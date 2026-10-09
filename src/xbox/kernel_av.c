/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_av.h for the contract and docs/av-policy.md for the evidence behind every
 * fabricated value below.
 */

#include "kernel_av.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "kernel_call.h"
#include "kernel_config.h"
#include "nt_status.h"

/* Encoder type for option 0x10. The title compares it against 2 and nothing else, so 1
 * is the answer that selects no encoder-specific workaround. Still a claim, still logged. */
#define FABRICATED_ENCODER_TYPE 1u

typedef struct {
    const char *name;
    uint32_t pack_code;
    uint32_t smc_video_mode;
} pack_entry;

/* Indexed by kernel_av_pack. SMC numbers: 1 is MEASURED (the only value under which the
 * title keeps 480p). The rest follow the console's SMC convention and the title never
 * tells them apart. */
static const pack_entry packs[KERNEL_AV_PACK_COUNT] = {
    [KERNEL_AV_PACK_COMPOSITE] = {"composite", KERNEL_AV_PACK_CODE_STANDARD, 6u},
    [KERNEL_AV_PACK_SVIDEO] = {"svideo", KERNEL_AV_PACK_CODE_SVIDEO, 4u},
    [KERNEL_AV_PACK_HDTV] = {"hdtv", KERNEL_AV_PACK_CODE_HDTV, 1u},
};

/* One mutex, recursive for the reason kernel_config.c gives: the log sink is supplied by
 * the caller and may itself re-enter. */
static pthread_mutex_t av_lock;
static bool av_lock_ready;
static pthread_once_t av_lock_once = PTHREAD_ONCE_INIT;

static void av_lock_init(void)
{
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        return;
    }
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0 &&
        pthread_mutex_init(&av_lock, &attr) == 0) {
        av_lock_ready = true;
    }
    (void)pthread_mutexattr_destroy(&attr);
}

static void lock(void)
{
    (void)pthread_once(&av_lock_once, av_lock_init);
    if (av_lock_ready) {
        (void)pthread_mutex_lock(&av_lock);
    }
}

static void unlock(void)
{
    if (av_lock_ready) {
        (void)pthread_mutex_unlock(&av_lock);
    }
}

static bool configured;
static kernel_av_pack chosen_pack;
static uint32_t chosen_standard;
static kernel_guest_ptr saved_data_address;
static kernel_av_display display;
static unsigned fabricated_count;
static unsigned refused_count;
static unsigned ignored_option_count;

kernel_av_pack kernel_av_default_pack(void)
{
    return KERNEL_AV_PACK_HDTV;
}

bool kernel_av_parse_pack(const char *name, kernel_av_pack *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    for (unsigned i = 0u; i < (unsigned)KERNEL_AV_PACK_COUNT; i++) {
        if (strcmp(name, packs[i].name) == 0) {
            *out = (kernel_av_pack)i;
            return true;
        }
    }
    return false;
}

const char *kernel_av_pack_name(kernel_av_pack pack)
{
    if ((unsigned)pack >= (unsigned)KERNEL_AV_PACK_COUNT) {
        return "?";
    }
    return packs[pack].name;
}

static const char *standard_name(uint32_t standard)
{
    switch (standard) {
    case KERNEL_AV_STANDARD_NTSC_M:
        return "NTSC-M";
    case KERNEL_AV_STANDARD_NTSC_J:
        return "NTSC-J";
    case KERNEL_AV_STANDARD_PAL_I:
        return "PAL-I";
    default:
        return "?";
    }
}

/* Caller holds the lock. */
static uint32_t flags_locked(void)
{
    uint32_t flags =
        chosen_standard == KERNEL_AV_STANDARD_PAL_I ? KERNEL_AV_FLAG_50HZ : KERNEL_AV_FLAG_60HZ;
    /* XGetVideoFlags strips the HDTV bits for PAL, so claiming 480p there contradicts the
     * title's own filter. */
    if (chosen_pack == KERNEL_AV_PACK_HDTV && chosen_standard != KERNEL_AV_STANDARD_PAL_I) {
        flags |= KERNEL_AV_FLAG_480P;
    }
    return flags;
}

static uint32_t standard_for_region(uint32_t region)
{
    if ((region & KERNEL_AV_REGION_NA) != 0u) {
        return KERNEL_AV_STANDARD_NTSC_M;
    }
    if ((region & KERNEL_AV_REGION_JAPAN) != 0u) {
        return KERNEL_AV_STANDARD_NTSC_J;
    }
    if ((region & KERNEL_AV_REGION_REST_OF_WORLD) != 0u) {
        return KERNEL_AV_STANDARD_PAL_I;
    }
    return 0u;
}

void kernel_av_reset(void)
{
    lock();
    configured = false;
    chosen_pack = KERNEL_AV_PACK_COMPOSITE;
    chosen_standard = 0u;
    saved_data_address = 0u;
    memset(&display, 0, sizeof(display));
    fabricated_count = 0u;
    refused_count = 0u;
    ignored_option_count = 0u;
    unlock();
}

bool kernel_av_configure(kernel_av_pack pack, uint32_t certificate_region)
{
    const uint32_t standard = standard_for_region(certificate_region);
    if ((unsigned)pack >= (unsigned)KERNEL_AV_PACK_COUNT || standard == 0u) {
        kernel_hle_log()("kernel: AV NOT configured: pack %d, certificate region 0x%08X. "
                         "A pack must be one of the named set and the region must carry "
                         "the NA, Japan or rest-of-world bit\n",
                         (int)pack, (unsigned)certificate_region);
        return false;
    }
    lock();
    chosen_pack = pack;
    chosen_standard = standard;
    configured = true;
    const uint32_t capabilities = packs[pack].pack_code | standard | flags_locked();
    unlock();
    kernel_hle_log()("kernel: AV pack is FABRICATED -- there is no AV encoder and no console. "
                     "The title is told: pack \"%s\" (code %u), standard %s (derived from "
                     "certificate region 0x%X), capability word 0x%08X. Chosen by --av-pack, "
                     "default \"%s\"\n",
                     packs[pack].name, (unsigned)packs[pack].pack_code, standard_name(standard),
                     (unsigned)certificate_region, (unsigned)capabilities,
                     packs[kernel_av_default_pack()].name);
    return true;
}

#define XBE_MAGIC 0x48454258u /* "XBEH" */
#define XBE_HEADER_SIZE_OF_HEADERS 0x108u
#define XBE_HEADER_CERTIFICATE_ADDRESS 0x118u
/* Bytes of certificate through the game-region word, the last field this module reads. */
#define XBE_CERTIFICATE_READ_BYTES (XBE_CERTIFICATE_GAME_REGION + 4u)
#define XBE_CERTIFICATE_GAME_REGION 0xA0u

bool kernel_av_configure_from_image(kernel_av_pack pack, kernel_guest_ptr image_base)
{
    uint32_t magic = 0u;
    uint32_t header_bytes = 0u;
    uint32_t certificate = 0u;
    uint32_t region = 0u;
    /* Bound the certificate to the declared header before reading it. Guest accessors
     * also reject unmapped pages; the header bound constrains which image bytes count. */
    const bool header_ok =
        kernel_guest_read_u32(image_base, &magic) && magic == XBE_MAGIC &&
        kernel_guest_read_u32(image_base + XBE_HEADER_SIZE_OF_HEADERS, &header_bytes) &&
        kernel_guest_read_u32(image_base + XBE_HEADER_CERTIFICATE_ADDRESS, &certificate);
    /* Unsigned wrap makes a certificate address below the image a huge offset, so one
     * comparison rejects both sides. */
    const uint32_t offset = certificate - image_base;
    const bool certificate_ok = header_ok && offset <= header_bytes &&
                                header_bytes - offset >= XBE_CERTIFICATE_READ_BYTES;
    if (!certificate_ok ||
        !kernel_guest_read_u32(certificate + XBE_CERTIFICATE_GAME_REGION, &region)) {
        kernel_hle_log()("kernel: AV NOT configured: no usable XBE header and certificate at "
                         "0x%08X, so the video standard cannot be derived from the region\n",
                         (unsigned)image_base);
        return false;
    }
    return kernel_av_configure(pack, region);
}

bool kernel_av_is_configured(void)
{
    lock();
    const bool value = configured;
    unlock();
    return value;
}

uint32_t kernel_av_capabilities(void)
{
    lock();
    uint32_t value = 0u;
    if (configured) {
        value = packs[chosen_pack].pack_code | chosen_standard | flags_locked();
    }
    unlock();
    return value;
}

uint32_t kernel_av_av_region_setting(void)
{
    lock();
    const uint32_t value = configured ? chosen_standard : 0u;
    unlock();
    return value;
}

uint32_t kernel_av_video_flags_setting(void)
{
    lock();
    const uint32_t value = configured ? (flags_locked() & KERNEL_AV_FLAG_480P) : 0u;
    unlock();
    return value;
}

uint32_t kernel_av_smc_video_mode(void)
{
    lock();
    const uint32_t value = configured ? packs[chosen_pack].smc_video_mode : 0u;
    unlock();
    return value;
}

unsigned kernel_av_install_settings(void)
{
    if (!kernel_av_is_configured()) {
        kernel_hle_log()("kernel: AV settings NOT installed: kernel_av_configure has not "
                         "succeeded\n");
        return 0u;
    }
    const uint32_t region_setting = kernel_av_av_region_setting();
    const uint32_t flags_setting = kernel_av_video_flags_setting();
    unsigned stored = 0u;
    if (kernel_config_set_setting(KERNEL_AV_SETTING_AV_REGION, &region_setting,
                                  (uint32_t)sizeof(region_setting))) {
        stored++;
    }
    if (kernel_config_set_setting(KERNEL_AV_SETTING_VIDEO_FLAGS, &flags_setting,
                                  (uint32_t)sizeof(flags_setting))) {
        stored++;
    }
    kernel_hle_log()("kernel: AV settings installed (%u of 2): ExQueryNonVolatileSetting "
                     "0x%X (AV region) = 0x%08X and 0x%X (video flags) = 0x%08X, FABRICATED "
                     "to agree with the option 6 answer\n",
                     stored, (unsigned)KERNEL_AV_SETTING_AV_REGION, (unsigned)region_setting,
                     (unsigned)KERNEL_AV_SETTING_VIDEO_FLAGS, (unsigned)flags_setting);
    return stored;
}

kernel_guest_ptr kernel_av_saved_data_address(void)
{
    lock();
    const kernel_guest_ptr value = saved_data_address;
    unlock();
    return value;
}

bool kernel_av_display_get(kernel_av_display *out)
{
    lock();
    const bool valid = display.valid;
    if (valid && out != NULL) {
        *out = display;
    }
    unlock();
    return valid;
}

unsigned kernel_av_fabricated_count(void)
{
    lock();
    const unsigned value = fabricated_count;
    unlock();
    return value;
}

unsigned kernel_av_refused_count(void)
{
    lock();
    const unsigned value = refused_count;
    unlock();
    return value;
}

unsigned kernel_av_ignored_option_count(void)
{
    lock();
    const unsigned value = ignored_option_count;
    unlock();
    return value;
}

static void count_refused(void)
{
    lock();
    refused_count++;
    unlock();
}

/* Fetch `count` stdcall arguments. A missing one is a refusal, never a substituted zero. */
static bool take_arguments(void *context, const char *who, uint32_t *args, unsigned count)
{
    for (unsigned i = 0u; i < count; i++) {
        if (!kernel_frame_arg((const kernel_call_frame *)context, i, &args[i])) {
            count_refused();
            kernel_hle_log()("kernel: %s could not read argument %u from the guest stack\n", who,
                             i);
            return false;
        }
    }
    return true;
}

static uint32_t hle_get_saved_data_address(void *context)
{
    (void)context;
    lock();
    const kernel_guest_ptr value = saved_data_address;
    unlock();
    kernel_hle_log()("kernel: AvGetSavedDataAddress() -> 0x%08X%s\n", (unsigned)value,
                     value == 0u ? " (no framebuffer left behind by a previous executable)"
                                 : "");
    return value;
}

static uint32_t hle_set_saved_data_address(void *context)
{
    uint32_t args[1];
    if (!take_arguments(context, "AvSetSavedDataAddress", args, 1u)) {
        return STATUS_INVALID_PARAMETER;
    }
    lock();
    saved_data_address = args[0];
    unlock();
    kernel_hle_log()("kernel: AvSetSavedDataAddress(0x%08X) recorded%s\n", (unsigned)args[0],
                     args[0] == 0u ? " (cleared)" : " -- NOT cleared, nothing maps it");
    return STATUS_SUCCESS;
}

static uint32_t hle_set_display_mode(void *context)
{
    uint32_t args[6];
    if (!take_arguments(context, "AvSetDisplayMode", args, 6u)) {
        return STATUS_INVALID_PARAMETER;
    }
    lock();
    display.valid = true;
    display.register_base = args[0];
    display.mode = args[2];
    display.format = args[3];
    display.pitch = args[4];
    display.frame_buffer = args[5];
    display.calls++;
    const unsigned calls = display.calls;
    unlock();
    /* Returning 0 ends the title's step loop. A nonzero step would send it round again
     * through a wait on a vertical-blank event nothing here ever signals. */
    kernel_hle_log()("kernel: AvSetDisplayMode(base 0x%08X, step %u, mode 0x%08X, format 0x%X, "
                     "pitch %u, framebuffer 0x%08X) call %u -> 0 (finished). RECORDED ONLY: no "
                     "CRTC or encoder is programmed, nothing is scanned out and no vertical "
                     "blank is waited for\n",
                     (unsigned)args[0], (unsigned)args[1], (unsigned)args[2], (unsigned)args[3],
                     (unsigned)args[4], (unsigned)args[5], calls);
    return 0u;
}

/* Write a fabricated query answer, or refuse when the guest gave nowhere to put it. */
static uint32_t answer_query(const char *what, uint32_t option, uint32_t result_ptr,
                             uint32_t value, const char *claim)
{
    /* kernel_guest_write_u32 rejects address 0, so a NULL result pointer lands here too. */
    if (!kernel_guest_write_u32(result_ptr, value)) {
        count_refused();
        kernel_hle_log()("kernel: AvSendTVEncoderOption(option 0x%X, %s) REFUSED -- result "
                         "pointer 0x%08X is unusable\n",
                         (unsigned)option, what, (unsigned)result_ptr);
        return STATUS_INVALID_PARAMETER;
    }
    lock();
    fabricated_count++;
    unlock();
    kernel_hle_log()("kernel: AvSendTVEncoderOption(option 0x%X, %s) -> 0x%08X written to "
                     "0x%08X. FABRICATED: %s\n",
                     (unsigned)option, what, (unsigned)value, (unsigned)result_ptr, claim);
    return STATUS_SUCCESS;
}

/* The Param values the title sends for each option, MEASURED from the push sequences at the
 * nine call sites (docs/av-policy.md section 2). Anything else is refused rather than
 * accepted: nothing is known about what the encoder would do with it. */
static bool param_is_measured(uint32_t option, uint32_t param)
{
    switch (option) {
    case KERNEL_AV_OPTION_BLANK_SCREEN:
        return param == 0u || param == 1u;
    case KERNEL_AV_OPTION_FLICKER_FILTER:
        return param == 0u || param == 5u;
    default:
        return param == 0u;
    }
}

static uint32_t refuse_option(uint32_t option, uint32_t param, const char *why)
{
    count_refused();
    kernel_hle_log()("kernel: AvSendTVEncoderOption(option 0x%X, param %u) REFUSED -- %s\n",
                     (unsigned)option, (unsigned)param, why);
    return STATUS_INVALID_PARAMETER;
}

/* Options 9, 0xB and 0xE: the title passes a NULL result at every site. */
static uint32_t accept_and_ignore_option(uint32_t option, uint32_t param, uint32_t result_ptr)
{
    if (result_ptr != 0u) {
        return refuse_option(option, param,
                             "the title passes a NULL result to this option at every site, and "
                             "it never answers, so a result pointer is not honoured");
    }
    lock();
    ignored_option_count++;
    unlock();
    kernel_hle_log()("kernel: AvSendTVEncoderOption(option 0x%X, param %u) accepted and "
                     "IGNORED -- there is no encoder to configure\n",
                     (unsigned)option, (unsigned)param);
    return STATUS_SUCCESS;
}

static uint32_t hle_send_tv_encoder_option(void *context)
{
    uint32_t args[4];
    if (!take_arguments(context, "AvSendTVEncoderOption", args, 4u)) {
        return STATUS_INVALID_PARAMETER;
    }
    const uint32_t option = args[1];
    const uint32_t param = args[2];
    const uint32_t result_ptr = args[3];

    switch (option) {
    case KERNEL_AV_OPTION_QUERY_AV_CAPABILITIES:
    case KERNEL_AV_OPTION_QUERY_FIELD:
    case KERNEL_AV_OPTION_QUERY_ENCODER_TYPE:
    case KERNEL_AV_OPTION_BLANK_SCREEN:
    case KERNEL_AV_OPTION_FLICKER_FILTER:
    case KERNEL_AV_OPTION_SOFT_DISPLAY_FILTER:
        if (!param_is_measured(option, param)) {
            return refuse_option(option, param,
                                 "the title is not known to send that param with this option, "
                                 "so nothing is claimed for it");
        }
        break;
    default:
        return refuse_option(option, param,
                             "this title is not known to send that option, so nothing is "
                             "claimed for it");
    }

    switch (option) {
    case KERNEL_AV_OPTION_QUERY_AV_CAPABILITIES: {
        if (!kernel_av_is_configured()) {
            count_refused();
            kernel_hle_log()("kernel: AvSendTVEncoderOption(option 6) REFUSED -- no AV pack has "
                             "been chosen. The host must call kernel_av_configure, so that a "
                             "hardware claim is never made by default\n");
            return STATUS_UNSUCCESSFUL;
        }
        return answer_query("query AV capabilities", option, result_ptr, kernel_av_capabilities(),
                            "the AV pack, video standard and display capabilities of a "
                            "console that does not exist");
    }
    case KERNEL_AV_OPTION_QUERY_FIELD:
        return answer_query("query current field", option, result_ptr, 0u,
                            "which video field the encoder is scanning, with no encoder");
    case KERNEL_AV_OPTION_QUERY_ENCODER_TYPE:
        return answer_query("query encoder type", option, result_ptr, FABRICATED_ENCODER_TYPE,
                            "the TV encoder chip, with no encoder (1 selects no chip-specific "
                            "workaround in the title)");
    default:
        return accept_and_ignore_option(option, param, result_ptr);
    }
}

unsigned kernel_av_register(void)
{
    static const struct {
        unsigned ordinal;
        kernel_fn handler;
    } bindings[] = {
        {KERNEL_AV_ORD_GET_SAVED_DATA, hle_get_saved_data_address},
        {KERNEL_AV_ORD_SEND_TV_ENCODER_OPTION, hle_send_tv_encoder_option},
        {KERNEL_AV_ORD_SET_DISPLAY_MODE, hle_set_display_mode},
        {KERNEL_AV_ORD_SET_SAVED_DATA, hle_set_saved_data_address},
    };

    unsigned bound = 0u;
    for (size_t i = 0u; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        if (kernel_hle_register(bindings[i].ordinal, bindings[i].handler)) {
            bound++;
        }
    }
    return bound;
}
