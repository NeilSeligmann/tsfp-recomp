/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_display.h. Every function carries the address of the original it ports.
 */

#include "d3d8_display.h"

#include <stdbool.h>

#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_surface.h"

/* AvSendTVEncoderOption, and the option the library sends for the capability query. */
#define KERNEL_ORDINAL_AV_SEND_TV_ENCODER_OPTION 2u
#define AV_OPTION_QUERY_CAPABILITIES 6u

/* Capability and row flag bits. The names are INFERRED from which rows each bit selects
 * (docs/av-policy.md section 3), the bit positions are MEASURED. */
#define FLAG_WIDESCREEN 0x00010000u
#define FLAG_HD_720P_1080I 0x00060000u
#define FLAG_HD_ANY 0x000E0000u
#define FLAG_REFRESH_CLASS 0x00C00000u
#define FLAG_60HZ 0x00400000u

#define PACK_HDTV 4u

uint32_t d3d8_display_capabilities(void)
{
    if (d3d8_guest_load32(D3D8_GLOBAL_AV_CAPABILITIES) == 0u) {
        /* AvSendTVEncoderOption(RegisterBase 0, option 6, param 0, &cache) at 0x003DBDB8.
         * The status is never read: the next instruction is `mov eax, [cache]`. */
        const uint32_t args[4] = {0u, AV_OPTION_QUERY_CAPABILITIES, 0u,
                                  D3D8_GLOBAL_AV_CAPABILITIES};
        (void)d3d8_kernel_call(KERNEL_ORDINAL_AV_SEND_TV_ENCODER_OPTION, args, 4u);
    }
    return d3d8_guest_load32(D3D8_GLOBAL_AV_CAPABILITIES);
}

static uint32_t row_address(uint32_t index)
{
    return D3D8_MODE_TABLE + index * D3D8_MODE_TABLE_ROW_BYTES;
}

uint32_t d3d8_display_mode_block(void)
{
    const uint32_t capabilities = d3d8_display_capabilities();
    const uint32_t standard = capabilities & 0xFF00u;
    const uint32_t pack = capabilities & 0xFFu;

    /* First loop (0x003DBDEC): the first row whose standard matches. All 185 rows are
     * checked, the terminator included, and a miss leaves the last row's address. */
    uint32_t index = 0u;
    uint32_t found = row_address(0u);
    for (;;) {
        found = row_address(index);
        if ((d3d8_guest_load32(found) & 0xFF00u) == standard) {
            break;
        }
        index++;
        if (index >= D3D8_MODE_TABLE_ROWS) {
            return found;
        }
    }

    /* Second loop (0x003DBE1A): from there, the first row whose pack is the requested one
     * or 0. Running off the end leaves the last row's address again. */
    for (;;) {
        found = row_address(index);
        const uint32_t row_pack = d3d8_guest_load32(found) & 0xFFu;
        if (row_pack == 0u || row_pack == pack) {
            return found;
        }
        index++;
        if (index >= D3D8_MODE_TABLE_ROWS) {
            return found;
        }
    }
}

uint32_t d3d8_display_present_flags(uint32_t row_flags)
{
    uint32_t flags = 0u;
    if ((row_flags & 0x00010000u) != 0u) {
        flags = 0x10u;
    }
    if ((row_flags & 0x00200000u) != 0u) {
        flags |= 0x20u;
    } else if ((row_flags & 0x01000000u) != 0u) {
        flags |= 0xA0u;
    } else {
        flags |= 0x40u;
    }
    if ((row_flags & 0x02000000u) != 0u) {
        flags |= 0x100u;
    }
    return flags;
}

/*
 * The row filter 0x003D9010 and 0x003D90B0 share, from the compare chain at 0x003D9057 and
 * 0x003D9103. They are two copies of the same tests with different outcomes, and this is
 * the one place those tests live.
 */
static bool row_accepted(uint32_t row_flags, uint32_t capabilities)
{
    const uint32_t pack = capabilities & 0xFFu;

    /* A widescreen row is dropped unless the answer is widescreen too, but a high-definition
     * row (720p or 1080i) under the HDTV pack skips that test and goes straight to the
     * refresh-class check. */
    const bool skip_widescreen_test = pack == PACK_HDTV && (row_flags & FLAG_HD_720P_1080I) != 0u;
    if (!skip_widescreen_test && (row_flags & FLAG_WIDESCREEN) != 0u &&
        (capabilities & FLAG_WIDESCREEN) == 0u) {
        return false;
    }
    if ((capabilities & FLAG_REFRESH_CLASS & row_flags) == 0u) {
        return false;
    }
    if (pack == PACK_HDTV) {
        if ((capabilities & FLAG_HD_ANY & row_flags) != 0u) {
            return true;
        }
        if ((row_flags & FLAG_HD_ANY) != 0u) {
            return false;
        }
    }
    return true;
}

uint32_t d3d8_adapter_mode_count(void)
{
    const uint32_t first = d3d8_display_mode_block();
    const uint32_t capabilities = d3d8_display_capabilities();
    const uint32_t block = d3d8_guest_load32(first) & 0xFFu;

    uint32_t accepted = 0u;
    uint32_t row = first;
    for (;;) {
        if (row_accepted(d3d8_guest_load32(row), capabilities)) {
            accepted++;
        }
        row += D3D8_MODE_TABLE_ROW_BYTES;
        /* The block ends at the first row whose pack byte differs. The terminator row
         * (0xFFFFFFFF) ends the last one. */
        if ((d3d8_guest_load32(row) & 0xFFu) != block) {
            break;
        }
    }
    return accepted << 2;
}

uint32_t d3d8_display_match_mode(uint32_t hw_object, const d3d8_mode_request *request)
{
    const uint32_t format = d3d8_surface_normalise_format(request->format);
    uint32_t row = d3d8_display_mode_block();
    const uint32_t capabilities = d3d8_display_capabilities();
    const uint32_t block = d3d8_guest_load32(row) & 0xFFu;
    const uint32_t pack = capabilities & 0xFFu;
    const uint32_t flags = request->flags;

    uint32_t refresh_class = capabilities & FLAG_REFRESH_CLASS;
    uint32_t scan = flags & 0x60u;
    if ((flags & 0x80u) != 0u) {
        scan &= ~0x20u;
    }
    const uint32_t hd_flags = pack == PACK_HDTV ? (capabilities & FLAG_HD_ANY) : 0u;

    /* 0x003DBEAF: the requested refresh rate picks which class a row must carry. */
    if (request->refresh == 50u) {
        if ((flags & 0x200u) == 0u || (capabilities & FLAG_60HZ) == 0u) {
            refresh_class = 0x00800000u;
        } else {
            d3d8_hle_fatal(0x003DBF61u, "a 50 Hz request with flag 0x200 on a 60 Hz capability "
                                        "word reprograms CRTC timing, which is not modelled");
        }
    } else if (request->refresh == 60u) {
        if ((flags & 0x200u) == 0u || (capabilities & 0x00800000u) == 0u) {
            refresh_class = FLAG_60HZ;
        } else {
            d3d8_hle_fatal(0x003DBECFu, "a 60 Hz request with flag 0x200 on a 50 Hz capability "
                                        "word reprograms CRTC timing, which is not modelled");
        }
    }

    uint32_t mode_word = 0u;
    if (pack != 0u) {
        for (;;) {
            const uint32_t row_flags = d3d8_guest_load32(row);
            if ((row_flags & 0xFFu) != block) {
                return D3D8_E_FAIL;
            }

            bool agrees = true;
            if (pack == PACK_HDTV && (hd_flags & row_flags) == 0u &&
                (row_flags & FLAG_HD_ANY) != 0u) {
                agrees = false;
            }
            const uint32_t size_word = d3d8_guest_load32(row + 4u);
            if (agrees && ((size_word & 0xFFFFu) != request->width ||
                           ((size_word >> 16) & 0xFFFFu) != request->height)) {
                agrees = false;
            }
            /* Three single-bit equalities between the row and the request: widescreen,
             * field and pixel aspect. */
            if (agrees && ((((row_flags >> 16) ^ (flags >> 4)) & 1u) != 0u ||
                           (((row_flags >> 24) ^ (flags >> 7)) & 1u) != 0u ||
                           (((row_flags >> 25) ^ (flags >> 8)) & 1u) != 0u)) {
                agrees = false;
            }
            if (agrees && (row_flags & refresh_class) == 0u) {
                agrees = false;
            }
            if (agrees && scan != 0u) {
                const bool row_interlaced = (row_flags & 0x00200000u) != 0u;
                if ((scan & 0x20u) != 0u && row_interlaced) {
                    /* interlaced requested and the row is */
                } else if ((scan & 0x40u) == 0u || row_interlaced) {
                    agrees = false;
                }
            }

            if (agrees) {
                /* A row that agrees on everything decides the call: a zero mode word or an
                 * unmet widescreen request is a failure here, not a reason to read on. */
                mode_word = d3d8_guest_load32(row + 8u);
                if (mode_word == 0u) {
                    return D3D8_E_FAIL;
                }
                /* The capability word must carry the class the row was matched on, and a
                 * widescreen request needs a widescreen capability word unless the row is a
                 * 720p or 1080i one under the HDTV pack, which is exempt from that second
                 * test (0x003DC05A). */
                bool admitted = (capabilities & refresh_class) != 0u;
                const bool hd_row = pack == PACK_HDTV && (row_flags & FLAG_HD_720P_1080I) != 0u;
                if (!hd_row && (flags & 0x10u) != 0u && (capabilities & FLAG_WIDESCREEN) == 0u) {
                    admitted = false;
                }
                if (!admitted) {
                    return D3D8_E_FAIL;
                }
                break;
            }
            row += D3D8_MODE_TABLE_ROW_BYTES;
        }
    }

    d3d8_guest_store32(hw_object + 0x0Cu, format);
    d3d8_guest_store32(hw_object + 0x04u, request->pitch);
    d3d8_guest_store32(hw_object + 0x08u, mode_word);
    const uint32_t slot = d3d8_guest_load32(hw_object + 0x7E4u);
    d3d8_guest_store32(hw_object + 0x7DCu + slot * 4u, 1u);
    d3d8_guest_store32(hw_object + 0x1B8u, 1u);
    d3d8_guest_store32(hw_object + 0x1B4u, d3d8_guest_load32(row));
    return 0u;
}

/* The format for each of the four variants of a row, from the jump table at 0x003D91F4
 * whose targets store these immediates (0x003D91A5, 0x003D91B8, 0x003D91CB, 0x003D91DE). */
static const uint32_t variant_format[4] = {0x1Eu, 0x11u, 0x1Cu, 0x12u};

uint32_t d3d8_adapter_enum_mode(uint32_t mode_index, uint32_t out_address)
{
    const uint32_t variant = mode_index & 3u;
    uint32_t remaining = mode_index >> 2;

    const uint32_t first = d3d8_display_mode_block();
    const uint32_t capabilities = d3d8_display_capabilities();
    const uint32_t block = d3d8_guest_load32(first) & 0xFFu;

    uint32_t row = first;
    for (;;) {
        const uint32_t row_flags = d3d8_guest_load32(row);
        if (row_accepted(row_flags, capabilities)) {
            if (remaining == 0u) {
                const uint32_t size_word = d3d8_guest_load32(row + 4u);
                d3d8_guest_store32(out_address, size_word & 0xFFFFu);
                d3d8_guest_store32(out_address + 4u, (size_word >> 16) & 0xFFFFu);
                d3d8_guest_store32(out_address + 8u, (row_flags & FLAG_60HZ) != 0u ? 60u : 50u);
                d3d8_guest_store32(out_address + 12u, d3d8_display_present_flags(row_flags));
                d3d8_guest_store32(out_address + 16u, variant_format[variant]);
                return 0u;
            }
            remaining--;
        }
        row += D3D8_MODE_TABLE_ROW_BYTES;
        if ((d3d8_guest_load32(row) & 0xFFu) != block) {
            return D3D8_E_INVALIDCALL;
        }
    }
}
