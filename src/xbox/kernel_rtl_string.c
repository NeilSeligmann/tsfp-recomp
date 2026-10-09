/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_rtl_string.h for the signatures, the measured sites and every INFERRED rule.
 */

#include "kernel_rtl_string.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "guest_structs.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "nt_status.h"

/* A counted string describes at most this many bytes (16-bit MaximumLength). */
#define COUNTED_STRING_MAX_BYTES 0xFFFFu

static uint32_t nonascii_count;

typedef enum {
    DIRECTION_ANSI_TO_UNICODE,
    DIRECTION_UNICODE_TO_ANSI,
} direction;

static bool read_descriptor(uint32_t address, guest_object_string *out)
{
    return kernel_guest_read_bytes(address, out, sizeof(*out));
}

/* The shared body. 260 and 308 differ only in the element widths and the mapping. */
static uint32_t convert(void *context, direction which, const char *who)
{
    if (context == NULL) {
        kernel_hle_log()("kernel: %s called with no argument frame\n", who);
        return STATUS_INVALID_PARAMETER;
    }
    uint32_t args[3];
    for (unsigned i = 0u; i < 3u; i++) {
        if (!kernel_frame_arg((const kernel_call_frame *)context, i, &args[i])) {
            kernel_hle_log()("kernel: %s could not read argument %u from the guest stack\n", who,
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    const uint32_t destination_address = args[0];
    const uint32_t source_address = args[1];
    if ((args[2] & 0xFFu) != 0u) {
        kernel_hle_log()("kernel: %s(%#x, %#x, %#x) REFUSED: AllocateDestinationString is "
                         "unmeasured (the one site passes the literal 0)\n",
                         who, destination_address, source_address, args[2]);
        return STATUS_NOT_IMPLEMENTED;
    }

    guest_object_string source;
    guest_object_string destination;
    if (!read_descriptor(source_address, &source) ||
        !read_descriptor(destination_address, &destination)) {
        kernel_hle_log()("kernel: %s(%#x, %#x) a string descriptor is not readable guest "
                         "memory\n",
                         who, destination_address, source_address);
        return STATUS_ACCESS_VIOLATION;
    }

    const bool widening = which == DIRECTION_ANSI_TO_UNICODE;
    if (!widening && (source.length & 1u) != 0u) {
        kernel_hle_log()("kernel: %s(%#x) REFUSED: the Unicode source Length %u is odd, "
                         "unmeasured\n",
                         who, source_address, (unsigned)source.length);
        return STATUS_INVALID_PARAMETER;
    }
    const uint32_t characters = widening ? source.length : (uint32_t)(source.length / 2u);
    const uint32_t converted_bytes = widening ? characters * 2u : characters;
    const uint32_t needed = converted_bytes + (widening ? 2u : 1u);
    if (needed > COUNTED_STRING_MAX_BYTES) {
        kernel_hle_log()("kernel: %s(%#x) REFUSED: %u characters need %u bytes, more than a "
                         "counted string can describe\n",
                         who, source_address, (unsigned)characters, (unsigned)needed);
        return STATUS_INVALID_PARAMETER;
    }
    if (destination.maximum_length < needed) {
        kernel_hle_log()("kernel: %s(%#x) the destination holds %u bytes and %u are needed, "
                         "STATUS_BUFFER_OVERFLOW\n",
                         who, destination_address, (unsigned)destination.maximum_length,
                         (unsigned)needed);
        return STATUS_BUFFER_OVERFLOW;
    }

    /* Read the source text, then compose the whole output on the host. */
    const uint32_t input_bytes = source.length;
    uint8_t *input = malloc((size_t)input_bytes + 1u);
    uint8_t *output = malloc(needed);
    if (input == NULL || output == NULL) {
        free(input);
        free(output);
        kernel_hle_log()("kernel: %s(%#x) ran out of host memory\n", who, source_address);
        return STATUS_NO_MEMORY;
    }
    if (input_bytes != 0u &&
        !kernel_guest_read_bytes(source.buffer, input, input_bytes)) {
        free(input);
        free(output);
        kernel_hle_log()("kernel: %s(%#x) the source text at %#x is not readable\n", who,
                         source_address, (unsigned)source.buffer);
        return STATUS_ACCESS_VIOLATION;
    }
    uint32_t replaced = 0u;
    if (widening) {
        for (uint32_t i = 0u; i < characters; i++) {
            output[2u * i] = input[i];
            output[2u * i + 1u] = 0u;
            if (input[i] >= 0x80u) {
                replaced++;
            }
        }
        output[converted_bytes] = 0u;
        output[converted_bytes + 1u] = 0u;
    } else {
        for (uint32_t i = 0u; i < characters; i++) {
            const uint32_t wide = (uint32_t)input[2u * i] | ((uint32_t)input[2u * i + 1u] << 8);
            output[i] = wide <= 0xFFu ? (uint8_t)wide : (uint8_t)'?';
            if (wide > 0xFFu) {
                replaced++;
            }
        }
        output[converted_bytes] = 0u;
    }
    const bool wrote = kernel_guest_write_bytes(destination.buffer, output, needed);
    free(input);
    free(output);
    if (!wrote) {
        kernel_hle_log()("kernel: %s(%#x) the destination text at %#x is not writable for %u "
                         "bytes\n",
                         who, destination_address, (unsigned)destination.buffer,
                         (unsigned)needed);
        return STATUS_ACCESS_VIOLATION;
    }
    const uint16_t length = (uint16_t)converted_bytes;
    if (!kernel_guest_write_bytes(destination_address, &length, sizeof(length))) {
        kernel_hle_log()("kernel: %s(%#x) the destination descriptor is not writable\n", who,
                         destination_address);
        return STATUS_ACCESS_VIOLATION;
    }
    if (replaced != 0u) {
        __atomic_fetch_add(&nonascii_count, replaced, __ATOMIC_RELAXED);
        kernel_hle_log()("kernel: %s converted %u %s -- the Latin-1 / '?' mapping is INFERRED, "
                         "not measured\n",
                         who, (unsigned)replaced,
                         widening ? "byte(s) at or above 0x80" : "character(s) above 0xFF");
    }
    return STATUS_SUCCESS;
}

static uint32_t hle_ansi_to_unicode(void *context)
{
    return convert(context, DIRECTION_ANSI_TO_UNICODE, "RtlAnsiStringToUnicodeString");
}

static uint32_t hle_unicode_to_ansi(void *context)
{
    return convert(context, DIRECTION_UNICODE_TO_ANSI, "RtlUnicodeStringToAnsiString");
}

uint32_t kernel_rtl_string_nonascii_count(void)
{
    return __atomic_load_n(&nonascii_count, __ATOMIC_RELAXED);
}

size_t kernel_rtl_string_register(void)
{
    nonascii_count = 0u;
    size_t registered = 0u;
    if (kernel_hle_register(ORD_RtlAnsiStringToUnicodeString, hle_ansi_to_unicode)) {
        registered++;
    }
    if (kernel_hle_register(ORD_RtlUnicodeStringToAnsiString, hle_unicode_to_ansi)) {
        registered++;
    }
    return registered;
}
