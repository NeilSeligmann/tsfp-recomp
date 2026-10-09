"""Mutations for the Av* ordinals (1 AvGetSavedDataAddress, 2 AvSendTVEncoderOption,
3 AvSetDisplayMode, 4 AvSetSavedDataAddress) in src/xbox/kernel_av.{c,h}.

WHAT THIS SET PROTECTS. The module hands the title a number describing a console that does not
exist, and every wrong number is quiet. A swapped pack code, a dropped refresh bit (the title's
mode count is ZERO without bit 0x400000) or a standard off by one byte makes the title select a
different display mode while the boot still advances. Entries are grouped by the property attacked:

  1. THE ORDINALS ARE BOUND        av-ord-get, av-ord-send, av-ord-mode, av-ord-set,
                                   av-register-swapped-handlers
  2. THE ANSWER                    av-pack-code-*, av-standard-*, av-flag-*, av-pal-keeps-480p,
                                   av-480p-not-hdtv-only, av-region-*, av-smc-*, av-encoder-type,
                                   av-field-answer
  3. WHAT IS REFUSED               av-unconfigured-answers, av-configure-*, av-image-*, av-cert-*,
                                   av-option-*, av-param-*, av-default-status,
                                   av-set-option-result-accepted, av-query-write-unchecked
  4. THE SHAPE OF EACH CALL        av-send-arity-*, av-mode-arity-*, av-set-saved-arity,
                                   av-mode-field-*, av-mode-step, av-mode-calls
  5. STATE                         av-saved-*, av-reset-*, av-count-*
  6. NOTHING IS SILENT             av-announce-*, av-settings-*

TWO FUNCTIONS HAVE NO MUTATION OF THEIR OWN, said here rather than left to be noticed.
`kernel_av_display_get`'s `valid &&` guard is redundant with the zeroed record, so removing it
changes no observable result. The log text of AvGetSavedDataAddress and AvSetSavedDataAddress is
not asserted: it is diagnostics, and a test that pinned prose would pin the wording, not the
behaviour.

`tsfp_host` is NOT a ctest binary, so nothing the host does with this module (the `--av-pack`
flag, the SMC word written into the thunk window, the order of configure and install) can be
mutated here. They are exercised by running the title instead.

THE ENTRIES USE `&& false` rather than `false`, per `_example.py`.
"""

AV_C = "src/xbox/kernel_av.c"
AV_H = "src/xbox/kernel_av.h"
SUITE = ["test_kernel_av"]


def entry(identifier: str, file: str, old: str, new: str, why: str) -> dict:
    return {
        "id": identifier,
        "file": file,
        "old": old,
        "new": new,
        "targets": SUITE,
        "why": why,
    }


MUTATIONS: list[dict] = [
    # ---- 1. the ordinals are bound ---------------------------------------------------------
    entry(
        "av-ord-get",
        AV_H,
        "#define KERNEL_AV_ORD_GET_SAVED_DATA 1u",
        "#define KERNEL_AV_ORD_GET_SAVED_DATA 5u",
        "a wrong ordinal binds the handler to a different export, so the title's 3 reads of the "
        "saved address stay stubs that return 0 and look like the right answer.",
    ),
    entry(
        "av-ord-send",
        AV_H,
        "#define KERNEL_AV_ORD_SEND_TV_ENCODER_OPTION 2u",
        "#define KERNEL_AV_ORD_SEND_TV_ENCODER_OPTION 6u",
        "the boot stops on ordinal 2 again, and a handler bound elsewhere is never called.",
    ),
    entry(
        "av-ord-mode",
        AV_H,
        "#define KERNEL_AV_ORD_SET_DISPLAY_MODE 3u",
        "#define KERNEL_AV_ORD_SET_DISPLAY_MODE 7u",
        "AvSetDisplayMode is the call that hands the title's scanout surface over, so an unbound "
        "one silently drops the framebuffer the GPU path needs.",
    ),
    entry(
        "av-ord-set",
        AV_H,
        "#define KERNEL_AV_ORD_SET_SAVED_DATA 4u",
        "#define KERNEL_AV_ORD_SET_SAVED_DATA 8u",
        "the title clears the saved address with this, so an unbound handler leaves a stale "
        "framebuffer address that the next Get reports.",
    ),
    entry(
        "av-register-swapped-handlers",
        AV_C,
        "        {KERNEL_AV_ORD_GET_SAVED_DATA, hle_get_saved_data_address},\n"
        "        {KERNEL_AV_ORD_SEND_TV_ENCODER_OPTION, hle_send_tv_encoder_option},\n"
        "        {KERNEL_AV_ORD_SET_DISPLAY_MODE, hle_set_display_mode},\n"
        "        {KERNEL_AV_ORD_SET_SAVED_DATA, hle_set_saved_data_address},\n",
        "        {KERNEL_AV_ORD_GET_SAVED_DATA, hle_set_saved_data_address},\n"
        "        {KERNEL_AV_ORD_SEND_TV_ENCODER_OPTION, hle_send_tv_encoder_option},\n"
        "        {KERNEL_AV_ORD_SET_DISPLAY_MODE, hle_set_display_mode},\n"
        "        {KERNEL_AV_ORD_SET_SAVED_DATA, hle_get_saved_data_address},\n",
        "a Get bound to the Set handler (and the reverse) answers the title's free of a left-over "
        "framebuffer with a store, and every handler is still bound so nothing reports a stub.",
    ),
    # ---- 2. the answer ---------------------------------------------------------------------
    entry(
        "av-pack-code-composite",
        AV_H,
        "#define KERNEL_AV_PACK_CODE_STANDARD 1u",
        "#define KERNEL_AV_PACK_CODE_STANDARD 2u",
        "pack 2 has no rows in the title's table, so the composite answer would still work "
        "while no longer being the code the table calls standard.",
    ),
    entry(
        "av-pack-code-hdtv",
        AV_H,
        "#define KERNEL_AV_PACK_CODE_HDTV 4u",
        "#define KERNEL_AV_PACK_CODE_HDTV 3u",
        "pack 3 is the SCART block: the title would be offered 480-line interlaced modes and "
        "never see the progressive ones it asks for.",
    ),
    entry(
        "av-pack-code-svideo",
        AV_H,
        "#define KERNEL_AV_PACK_CODE_SVIDEO 6u",
        "#define KERNEL_AV_PACK_CODE_SVIDEO 1u",
        "S-Video collapsing onto composite is invisible to the title but not to the operator's "
        "choice.",
    ),
    entry(
        "av-standard-ntsc-m",
        AV_H,
        "#define KERNEL_AV_STANDARD_NTSC_M 0x00000100u",
        "#define KERNEL_AV_STANDARD_NTSC_M 0x00000400u",
        "the standard is bits 8-15 and the title's table is keyed on them, and 0x400 is PAL-M, "
        "which the table has NO rows for: every lookup lands on the terminator row.",
    ),
    entry(
        "av-standard-pal",
        AV_H,
        "#define KERNEL_AV_STANDARD_PAL_I 0x00000300u",
        "#define KERNEL_AV_STANDARD_PAL_I 0x00000400u",
        "0x400 is PAL-M, which the title's table has NO rows for: every lookup lands on the "
        "terminator row.",
    ),
    entry(
        "av-flag-60hz",
        AV_H,
        "#define KERNEL_AV_FLAG_60HZ 0x00400000u",
        "#define KERNEL_AV_FLAG_60HZ 0x00000000u",
        "without bit 0x400000 the title's adapter-mode count is ZERO and every mode check "
        "fails, with no kernel call to show it.",
    ),
    entry(
        "av-flag-50hz",
        AV_H,
        "#define KERNEL_AV_FLAG_50HZ 0x00800000u",
        "#define KERNEL_AV_FLAG_50HZ 0x00400000u",
        "a PAL console answering 60 Hz is offered the wrong refresh class.",
    ),
    entry(
        "av-flag-480p",
        AV_H,
        "#define KERNEL_AV_FLAG_480P 0x00080000u",
        "#define KERNEL_AV_FLAG_480P 0x00040000u",
        "0x40000 is the 1080i bit: the title would be offered 1920 by 1080 instead of the "
        "progressive 640 by 480 it requests.",
    ),
    entry(
        "av-pal-keeps-480p",
        AV_C,
        "if (chosen_pack == KERNEL_AV_PACK_HDTV && chosen_standard != KERNEL_AV_STANDARD_PAL_I) {",
        "if (chosen_pack == KERNEL_AV_PACK_HDTV && true) {",
        "XGetVideoFlags strips 480p for PAL, so claiming it contradicts the title's own filter "
        "and the three routes to the claim disagree.",
    ),
    entry(
        "av-480p-not-hdtv-only",
        AV_C,
        "if (chosen_pack == KERNEL_AV_PACK_HDTV && chosen_standard !=",
        "if (chosen_pack != KERNEL_AV_PACK_COMPOSITE && chosen_standard !=",
        "only the component pack carries 480p in the title's own gate, so granting it to "
        "S-Video claims a capability that pack lacks.",
    ),
    entry(
        "av-region-na-first",
        AV_C,
        "    if ((region & KERNEL_AV_REGION_NA) != 0u) {",
        "    if ((region & KERNEL_AV_REGION_NA) != 0u &&"
        " (region & KERNEL_AV_REGION_JAPAN) == 0u) {",
        "a title released in NA and Japan must run as NTSC-M, so the priority order is the "
        "behaviour.",
    ),
    entry(
        "av-region-japan-before-row",
        AV_C,
        "    if ((region & KERNEL_AV_REGION_JAPAN) != 0u) {",
        "    if ((region & KERNEL_AV_REGION_JAPAN) != 0u && (region & "
        "KERNEL_AV_REGION_REST_OF_WORLD) == 0u) {",
        "Japan before rest-of-world is the second half of the priority order.",
    ),
    entry(
        "av-region-row-standard",
        AV_C,
        "        return KERNEL_AV_STANDARD_PAL_I;\n    }\n    return 0u;",
        "        return KERNEL_AV_STANDARD_NTSC_M;\n    }\n    return 0u;",
        "a rest-of-world title would be told NTSC.",
    ),
    entry(
        "av-smc-hdtv",
        AV_C,
        '[KERNEL_AV_PACK_HDTV] = {"hdtv", KERNEL_AV_PACK_CODE_HDTV, 1u},',
        '[KERNEL_AV_PACK_HDTV] = {"hdtv", KERNEL_AV_PACK_CODE_HDTV, 2u},',
        "SMC video mode 1 is the ONLY value under which XGetVideoFlags keeps 480p, measured, so "
        "any other value silently disables progressive output.",
    ),
    entry(
        "av-smc-composite",
        AV_C,
        '[KERNEL_AV_PACK_COMPOSITE] = {"composite", KERNEL_AV_PACK_CODE_STANDARD, 6u},',
        '[KERNEL_AV_PACK_COMPOSITE] = {"composite", KERNEL_AV_PACK_CODE_STANDARD, 1u},',
        "composite reading as the HDTV SMC value would keep 480p for a pack that has none.",
    ),
    entry(
        "av-smc-unconfigured",
        AV_C,
        "    const uint32_t value = configured ? packs[chosen_pack].smc_video_mode : 0u;",
        "    const uint32_t value = packs[chosen_pack].smc_video_mode;",
        "an unconfigured module must claim no SMC video mode at all.",
    ),
    entry(
        "av-encoder-type",
        AV_C,
        "#define FABRICATED_ENCODER_TYPE 1u",
        "#define FABRICATED_ENCODER_TYPE 2u",
        "the title compares the encoder type with 2 and applies a chip-specific 480p workaround "
        "on a match, so answering 2 changes what the title does.",
    ),
    entry(
        "av-field-answer",
        AV_C,
        '"query current field", option, result_ptr, 0u,',
        '"query current field", option, result_ptr, 1u,',
        "the title keeps a field counter in step with this, and a nonzero answer shifts its "
        "parity.",
    ),
    # ---- 3. what is refused ----------------------------------------------------------------
    entry(
        "av-unconfigured-answers",
        AV_C,
        "        if (!kernel_av_is_configured()) {\n            count_refused();",
        "        if (!kernel_av_is_configured() && false) {\n            count_refused();",
        "answering option 6 before the operator's choice is a hardware claim nobody made.",
    ),
    entry(
        "av-unconfigured-capabilities",
        AV_C,
        "    if (configured) {\n        value = packs[chosen_pack].pack_code",
        "    if (configured && false) {\n        value = packs[chosen_pack].pack_code",
        "a configured module that answers 0 gives the title pack 0 and standard 0, which lands "
        "on the terminator row.",
    ),
    entry(
        "av-configure-pack-range",
        AV_C,
        "if ((unsigned)pack >= (unsigned)KERNEL_AV_PACK_COUNT || standard == 0u) {",
        "if ((unsigned)pack > (unsigned)KERNEL_AV_PACK_COUNT || standard == 0u) {",
        "an off-by-one lets pack COUNT index one past the table.",
    ),
    entry(
        "av-configure-no-region",
        AV_C,
        "if ((unsigned)pack >= (unsigned)KERNEL_AV_PACK_COUNT || standard == 0u) {",
        "if ((unsigned)pack >= (unsigned)KERNEL_AV_PACK_COUNT || standard == 1u) {",
        "a region word with no region bit must not configure a standard of 0.",
    ),
    entry(
        "av-configure-state",
        AV_C,
        "    chosen_standard = standard;\n    configured = true;",
        "    chosen_standard = standard;\n    configured = false;",
        "a configure that never sets the flag leaves every query refused.",
    ),
    entry(
        "av-image-magic",
        AV_C,
        "kernel_guest_read_u32(image_base, &magic) && magic == XBE_MAGIC &&",
        "kernel_guest_read_u32(image_base, &magic) && (magic == XBE_MAGIC || true) &&",
        "reading a certificate out of memory that is not an XBE header derives a region from "
        "noise.",
    ),
    entry(
        "av-cert-address-offset",
        AV_C,
        "#define XBE_HEADER_CERTIFICATE_ADDRESS 0x118u",
        "#define XBE_HEADER_CERTIFICATE_ADDRESS 0x11Cu",
        "header +0x11C is the section count, so the region would be read from a section header.",
    ),
    entry(
        "av-cert-region-offset",
        AV_C,
        "#define XBE_CERTIFICATE_GAME_REGION 0xA0u",
        "#define XBE_CERTIFICATE_GAME_REGION 0xA4u",
        "certificate +0xA4 is the game ratings, a different word that is also a small integer.",
    ),
    entry(
        "av-cert-region-offset-media",
        AV_C,
        "#define XBE_CERTIFICATE_GAME_REGION 0xA0u",
        "#define XBE_CERTIFICATE_GAME_REGION 0x9Cu",
        "certificate +0x9C is the allowed media, the other neighbour of the region word.",
    ),
    entry(
        "av-image-header-size",
        AV_C,
        "#define XBE_HEADER_SIZE_OF_HEADERS 0x108u",
        "#define XBE_HEADER_SIZE_OF_HEADERS 0x10Cu",
        "header +0x10C is SizeOfImage, so the bound would accept certificate bytes outside "
        "the declared header; guest accessors independently reject unmapped pages.",
    ),
    entry(
        "av-image-range-upper",
        AV_C,
        "const bool certificate_ok = header_ok && offset <= header_bytes &&",
        "const bool certificate_ok = header_ok && (offset <= header_bytes || true) &&",
        "without the upper bound a certificate address past the header underflows the room "
        "left and is accepted.",
    ),
    entry(
        "av-image-range-room",
        AV_C,
        "header_bytes - offset >= XBE_CERTIFICATE_READ_BYTES;",
        "header_bytes - offset >= 0u;",
        "a certificate too close to the header's end has its region word outside the header.",
    ),
    entry(
        "av-image-room-off-by-four",
        AV_C,
        "#define XBE_CERTIFICATE_READ_BYTES (XBE_CERTIFICATE_GAME_REGION + 4u)",
        "#define XBE_CERTIFICATE_READ_BYTES (XBE_CERTIFICATE_GAME_REGION + 0u)",
        "the region word is four bytes wide, and the last one must fit.",
    ),
    entry(
        "av-option-6-number",
        AV_H,
        "#define KERNEL_AV_OPTION_QUERY_AV_CAPABILITIES 6u",
        "#define KERNEL_AV_OPTION_QUERY_AV_CAPABILITIES 7u",
        "option 7 is not sent by the title, so the real query would be refused as unknown.",
    ),
    entry(
        "av-option-blank-number",
        AV_H,
        "#define KERNEL_AV_OPTION_BLANK_SCREEN 9u",
        "#define KERNEL_AV_OPTION_BLANK_SCREEN 8u",
        "the title's blank-screen call would be refused and its stop unexplained.",
    ),
    entry(
        "av-option-flicker-number",
        AV_H,
        "#define KERNEL_AV_OPTION_FLICKER_FILTER 0xBu",
        "#define KERNEL_AV_OPTION_FLICKER_FILTER 0xCu",
        "same, for SetFlickerFilter.",
    ),
    entry(
        "av-option-soft-number",
        AV_H,
        "#define KERNEL_AV_OPTION_SOFT_DISPLAY_FILTER 0xEu",
        "#define KERNEL_AV_OPTION_SOFT_DISPLAY_FILTER 0xDu",
        "same, for SetSoftDisplayFilter.",
    ),
    entry(
        "av-option-field-number",
        AV_H,
        "#define KERNEL_AV_OPTION_QUERY_FIELD 0xFu",
        "#define KERNEL_AV_OPTION_QUERY_FIELD 0x11u",
        "the field query would be refused as unknown.",
    ),
    entry(
        "av-option-encoder-number",
        AV_H,
        "#define KERNEL_AV_OPTION_QUERY_ENCODER_TYPE 0x10u",
        "#define KERNEL_AV_OPTION_QUERY_ENCODER_TYPE 0x12u",
        "the encoder query would be refused as unknown.",
    ),
    entry(
        "av-option-widened",
        AV_C,
        "    case KERNEL_AV_OPTION_SOFT_DISPLAY_FILTER:\n        if (!param_is_measured",
        "    case KERNEL_AV_OPTION_SOFT_DISPLAY_FILTER:\n    case 0xCu:\n        if "
        "(!param_is_measured",
        "an option the title is not known to send must be refused, and 0xC sits directly beside "
        "two it does.",
    ),
    entry(
        "av-option-widened-low",
        AV_C,
        "    case KERNEL_AV_OPTION_QUERY_ENCODER_TYPE:\n    case KERNEL_AV_OPTION_BLANK_SCREEN:\n",
        "    case KERNEL_AV_OPTION_QUERY_ENCODER_TYPE:\n    case 8u:\n    case "
        "KERNEL_AV_OPTION_BLANK_SCREEN:\n",
        "option 8 is the neighbour below blank-screen.",
    ),
    entry(
        "av-default-status",
        AV_C,
        "    return STATUS_INVALID_PARAMETER;\n}\n\n/* Options 9, 0xB and 0xE",
        "    return STATUS_SUCCESS;\n}\n\n/* Options 9, 0xB and 0xE",
        "a refusal the guest reads as success is a claim the module did not make. This is the "
        "one exit of every unknown option and every unmeasured param.",
    ),
    entry(
        "av-set-option-result-accepted",
        AV_C,
        "    if (result_ptr != 0u) {\n        return refuse_option(option, param,\n"
        '                             "the title passes a NULL result',
        "    if (result_ptr != 0u && false) {\n        return refuse_option(option, param,\n"
        '                             "the title passes a NULL result',
        "a set-option handed a real result pointer is accepted with the slot left stale, so the "
        "caller reads an answer that was never written.",
    ),
    entry(
        "av-param-check-dropped",
        AV_C,
        "        if (!param_is_measured(option, param)) {",
        "        if (!param_is_measured(option, param) && false) {",
        "no param is ever refused, so a value the title never sends is accepted as measured.",
    ),
    entry(
        "av-param-blank-widened",
        AV_C,
        "        return param == 0u || param == 1u;",
        "        return param <= 2u;",
        "option 9 is measured with 0 and 1 only.",
    ),
    entry(
        "av-param-blank-narrowed",
        AV_C,
        "        return param == 0u || param == 1u;",
        "        return param == 0u;",
        "option 9 with param 1 is sent at 0x003DA5EC and 0x003DD587.",
    ),
    entry(
        "av-param-flicker-widened",
        AV_C,
        "        return param == 0u || param == 5u;",
        "        return param <= 5u;",
        "option 0xB is measured with 0 and 5 only.",
    ),
    entry(
        "av-param-flicker-narrowed",
        AV_C,
        "        return param == 0u || param == 5u;",
        "        return param == 5u;",
        "option 0xB with param 0 is sent at 0x003D8563.",
    ),
    entry(
        "av-param-flicker-five-wrong",
        AV_C,
        "        return param == 0u || param == 5u;",
        "        return param == 0u || param == 4u;",
        "SetFlickerFilter(5) at 0x003DB14B is the title's only nonzero flicker value.",
    ),
    entry(
        "av-param-default-widened",
        AV_C,
        "    default:\n        return param == 0u;",
        "    default:\n        return param <= 1u;",
        "the queries and option 0xE are measured with param 0 only.",
    ),
    entry(
        "av-param-soft-unchecked",
        AV_C,
        "    default:\n        return param == 0u;",
        "    case KERNEL_AV_OPTION_SOFT_DISPLAY_FILTER:\n        return true;\n    default:\n"
        "        return param == 0u;",
        "option 0xE alone is left unchecked: SetSoftDisplayFilter has one caller, with 0.",
    ),
    entry(
        "av-param-field-unchecked",
        AV_C,
        "    default:\n        return param == 0u;",
        "    case KERNEL_AV_OPTION_QUERY_FIELD:\n        return true;\n    default:\n"
        "        return param == 0u;",
        "option 0xF alone is left unchecked.",
    ),
    entry(
        "av-param-encoder-unchecked",
        AV_C,
        "    default:\n        return param == 0u;",
        "    case KERNEL_AV_OPTION_QUERY_ENCODER_TYPE:\n        return true;\n    default:\n"
        "        return param == 0u;",
        "option 0x10 alone is left unchecked.",
    ),
    entry(
        "av-param-capabilities-unchecked",
        AV_C,
        "    default:\n        return param == 0u;",
        "    case KERNEL_AV_OPTION_QUERY_AV_CAPABILITIES:\n        return true;\n    default:\n"
        "        return param == 0u;",
        "option 6 alone is left unchecked.",
    ),
    entry(
        "av-query-write-unchecked",
        AV_C,
        "    if (!kernel_guest_write_u32(result_ptr, value)) {",
        "    if (kernel_guest_write_u32(result_ptr, value) && false) {",
        "a query with no result pointer must be refused, not counted as an answer.",
    ),
    # ---- 4. the shape of each call ---------------------------------------------------------
    entry(
        "av-send-arity-short",
        AV_C,
        '    if (!take_arguments(context, "AvSendTVEncoderOption", args, 4u)) {',
        '    if (!take_arguments(context, "AvSendTVEncoderOption", args, 3u)) {',
        "the call takes four arguments, and a count of three leaves the result pointer unread "
        "and the guest's esp wrong by four bytes forever.",
    ),
    entry(
        "av-send-arity-long",
        AV_C,
        "    uint32_t args[4];\n"
        '    if (!take_arguments(context, "AvSendTVEncoderOption", args, 4u)) {',
        "    uint32_t args[5];\n"
        '    if (!take_arguments(context, "AvSendTVEncoderOption", args, 5u)) {',
        "a count of five reads a slot the caller never pushed.",
    ),
    entry(
        "av-send-argument-order",
        AV_C,
        "    const uint32_t option = args[1];",
        "    const uint32_t option = args[2];",
        "the option is the SECOND argument and Param the third, and swapping them answers the "
        "wrong question.",
    ),
    entry(
        "av-send-result-slot",
        AV_C,
        "    const uint32_t result_ptr = args[3];",
        "    const uint32_t result_ptr = args[2];",
        "the result pointer is the LAST argument, after a Param of zero.",
    ),
    entry(
        "av-mode-arity-short",
        AV_C,
        '    if (!take_arguments(context, "AvSetDisplayMode", args, 6u)) {',
        '    if (!take_arguments(context, "AvSetDisplayMode", args, 5u)) {',
        "six arguments, and the sixth is the framebuffer the GPU path needs.",
    ),
    entry(
        "av-mode-arity-long",
        AV_C,
        '    uint32_t args[6];\n    if (!take_arguments(context, "AvSetDisplayMode", args, 6u)) {',
        '    uint32_t args[7];\n    if (!take_arguments(context, "AvSetDisplayMode", args, 7u)) {',
        "a count of seven reads past the caller's arguments.",
    ),
    entry(
        "av-mode-field-mode",
        AV_C,
        "    display.mode = args[2];",
        "    display.mode = args[3];",
        "Mode and Format are adjacent and both small integers, so a swap is invisible by eye.",
    ),
    entry(
        "av-mode-field-format",
        AV_C,
        "    display.format = args[3];",
        "    display.format = args[2];",
        "the other half of the same swap.",
    ),
    entry(
        "av-mode-field-pitch",
        AV_C,
        "    display.pitch = args[4];",
        "    display.pitch = args[5];",
        "Pitch and FrameBuffer swapped would hand the GPU path a pitch where an address belongs.",
    ),
    entry(
        "av-mode-field-framebuffer",
        AV_C,
        "    display.frame_buffer = args[5];",
        "    display.frame_buffer = args[4];",
        "the framebuffer address is the whole point of recording the call.",
    ),
    entry(
        "av-mode-field-base",
        AV_C,
        "    display.register_base = args[0];",
        "    display.register_base = args[1];",
        "the register base is the first argument and Step the second.",
    ),
    entry(
        "av-mode-step",
        AV_C,
        "                     (unsigned)args[4], (unsigned)args[5], calls);\n    return 0u;",
        "                     (unsigned)args[4], (unsigned)args[5], calls);\n    return 1u;",
        "a nonzero step sends the title round its loop again, through a wait on a vertical "
        "blank nothing signals: a hang.",
    ),
    entry(
        "av-mode-calls",
        AV_C,
        "    display.calls++;",
        "    display.calls += 0u;",
        "the call count is how a caller tells the pre-clear from the real mode.",
    ),
    entry(
        "av-mode-valid",
        AV_C,
        "    display.valid = true;",
        "    display.valid = false;",
        "a record never marked valid reports no display mode to the GPU path.",
    ),
    entry(
        "av-set-saved-arity",
        AV_C,
        '    if (!take_arguments(context, "AvSetSavedDataAddress", args, 1u)) {',
        '    if (!take_arguments(context, "AvSetSavedDataAddress", args, 0u)) {',
        "the single call site is the one the 3-site quorum cannot vouch for, so the count is "
        "pinned here from both sides.",
    ),
    # ---- 5. state --------------------------------------------------------------------------
    entry(
        "av-saved-store",
        AV_C,
        "    saved_data_address = args[0];",
        "    saved_data_address = 0u;",
        "a stored address that reads back as 0 turns every set into a clear.",
    ),
    entry(
        "av-saved-get-return",
        AV_C,
        '                                 : "");\n    return value;',
        '                                 : "");\n    return 0u;',
        "the title frees whatever framebuffer this reports, so a Get that always says 0 hides "
        "a stored one.",
    ),
    entry(
        "av-reset-saved",
        AV_C,
        "    saved_data_address = 0u;\n    memset(&display, 0, sizeof(display));",
        "    memset(&display, 0, sizeof(display));",
        "a reset that keeps the saved address leaks one run's framebuffer into the next.",
    ),
    entry(
        "av-reset-display",
        AV_C,
        "    saved_data_address = 0u;\n    memset(&display, 0, sizeof(display));",
        "    saved_data_address = 0u;",
        "a reset that keeps the display record leaks one run's scanout surface into the next.",
    ),
    entry(
        "av-reset-fabricated",
        AV_C,
        "    fabricated_count = 0u;\n    refused_count = 0u;",
        "    refused_count = 0u;",
        "a count of fabricated answers carried across runs overstates how much was claimed.",
    ),
    entry(
        "av-reset-refused",
        AV_C,
        "    refused_count = 0u;\n    ignored_option_count = 0u;\n    unlock();",
        "    ignored_option_count = 0u;\n    unlock();",
        "the refusal count is how a bring-up run learns the title asked for something unknown.",
    ),
    entry(
        "av-reset-ignored",
        AV_C,
        "    ignored_option_count = 0u;\n    unlock();\n}",
        "    unlock();\n}",
        "the ignored-option count is the measure of how much of the encoder is not modelled.",
    ),
    entry(
        "av-reset-configured",
        AV_C,
        "    configured = false;\n    chosen_pack = KERNEL_AV_PACK_COMPOSITE;",
        "    chosen_pack = KERNEL_AV_PACK_COMPOSITE;",
        "a reset that stays configured keeps making the previous run's hardware claim.",
    ),
    entry(
        "av-count-fabricated",
        AV_C,
        "    fabricated_count++;",
        "    fabricated_count += 0u;",
        "the fabricated count is the run's statement of how much it invented.",
    ),
    entry(
        "av-count-ignored",
        AV_C,
        "    ignored_option_count++;",
        "    ignored_option_count += 0u;",
        "the same for options accepted with no effect.",
    ),
    entry(
        "av-count-refused",
        AV_C,
        "    refused_count++;\n    unlock();\n}\n\n/* Fetch",
        "    refused_count += 0u;\n    unlock();\n}\n\n/* Fetch",
        "a refusal that is not counted is invisible in the end-of-run report.",
    ),
    entry(
        "av-default-pack",
        AV_C,
        "    return KERNEL_AV_PACK_HDTV;",
        "    return KERNEL_AV_PACK_COMPOSITE;",
        "the default is derived from the title's highest requested mode (480p), not chosen for "
        "convenience.",
    ),
    entry(
        "av-parse-prefix",
        AV_C,
        "        if (strcmp(name, packs[i].name) == 0) {",
        "        if (strncmp(name, packs[i].name, 3u) == 0) {",
        "a prefix match accepts names outside the bounded set.",
    ),
    entry(
        "av-parse-null",
        AV_C,
        "    if (name == NULL || out == NULL) {",
        "    if (name == NULL && out == NULL) {",
        "a NULL name or output must be refused, not dereferenced.",
    ),
    entry(
        "av-pack-name-range",
        AV_C,
        '    if ((unsigned)pack >= (unsigned)KERNEL_AV_PACK_COUNT) {\n        return "?";',
        '    if ((unsigned)pack > (unsigned)KERNEL_AV_PACK_COUNT) {\n        return "?";',
        "an off-by-one reads one name past the table.",
    ),
    # ---- 6. nothing is silent --------------------------------------------------------------
    entry(
        "av-announce-fabricated",
        AV_C,
        '"kernel: AV pack is FABRICATED -- there is no AV encoder and no console. "',
        '"kernel: AV pack is set -- there is an AV encoder and a console. "',
        "'nothing hardware-claiming may be silent' is the policy, so the announcement is the "
        "behaviour.",
    ),
    entry(
        "av-announce-standard",
        AV_C,
        '        return "NTSC-M";',
        '        return "NTSC-J";',
        "the announcement must name the standard the title was told.",
    ),
    entry(
        "av-announce-query",
        AV_C,
        '"0x%08X. FABRICATED: %s\\n",',
        '"0x%08X. %s\\n",',
        "each answer, not only the configuration, must say that it is invented.",
    ),
    entry(
        "av-announce-mode",
        AV_C,
        'pitch %u, framebuffer 0x%08X) call %u -> 0 (finished). RECORDED ONLY: no "',
        'pitch %u, framebuffer 0x%08X) call %u -> 0 (finished). recorded: no "',
        "the mode set programs nothing, and the log must say so in terms a grep finds.",
    ),
    entry(
        "av-announce-ignored",
        AV_C,
        'accepted and "\n                     "IGNORED -- there is no encoder to configure\\n",',
        'accepted and "\n                     "applied -- there is no encoder to configure\\n",',
        "an option with no effect must not read as one that worked.",
    ),
    entry(
        "av-settings-guard",
        AV_C,
        '    if (!kernel_av_is_configured()) {\n        kernel_hle_log()("kernel: AV settings NOT',
        "    if (!kernel_av_is_configured() && false) {\n"
        '        kernel_hle_log()("kernel: AV settings NOT',
        "installing settings for an unconfigured module writes zeros as if they were a claim.",
    ),
    entry(
        "av-settings-region-index",
        AV_H,
        "#define KERNEL_AV_SETTING_AV_REGION 0x103u",
        "#define KERNEL_AV_SETTING_AV_REGION 0x104u",
        "index 0x104 is the game region: the title's XGetVideoStandard reads 0x103.",
    ),
    entry(
        "av-settings-flags-index",
        AV_H,
        "#define KERNEL_AV_SETTING_VIDEO_FLAGS 8u",
        "#define KERNEL_AV_SETTING_VIDEO_FLAGS 9u",
        "index 9 is the audio flags: the title's XGetVideoFlags reads 8.",
    ),
    entry(
        "av-settings-flags-value",
        AV_C,
        "(flags_locked() & KERNEL_AV_FLAG_480P) : 0u;",
        "flags_locked() : 0u;",
        "the video-flags setting carries the user's 480p choice only, and including the "
        "refresh bit puts a 60 Hz bit where XGetVideoFlags reads PAL60.",
    ),
    entry(
        "av-settings-count",
        AV_C,
        "(uint32_t)sizeof(region_setting))) {\n        stored++;",
        "(uint32_t)sizeof(region_setting))) {\n        stored += 2u;",
        "the installed count is what the host reports, and it must not be overstated.",
    ),
]
