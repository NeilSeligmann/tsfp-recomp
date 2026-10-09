/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_dbg.h for the signature, the measured call shape, and why the handler
 * never pops anything: DbgPrint is __cdecl and the caller cleans up, so the thunk's
 * `{8u, THUNK_CC_CDECL, 0u, 0u}` row already does the whole cleanup (return address
 * only). This file is fetch-and-report with every guest read bounded.
 */

#include "kernel_dbg.h"

#include <stdio.h>
#include <string.h>

#include "kernel_hle.h"

/* Counters are plain words, written from the dispatcher's thread. The same lost-update
 * caveat as thunk_trace's announce bitmap applies: losing a race costs a miscount of a
 * diagnostic counter, not a wrong answer to the guest. */
static unsigned print_count;
static unsigned refused_count;

/* Append one byte if room. `*used` counts bytes written, excluding the NUL. */
static void put_char(char *out, size_t out_size, size_t *used, char value)
{
    if (*used + 1u < out_size) {
        out[*used] = value;
        (*used)++;
    }
}

static void put_text(char *out, size_t out_size, size_t *used, const char *text)
{
    while (*text != '\0') {
        put_char(out, out_size, used, *text);
        text++;
    }
}

/* The next variadic argument, or false. Slot 0 of the frame is Format itself, so the
 * cursor starts at 1 and only a RECOGNISED specifier advances it -- an unknown one
 * must not, or a format the title never meant as a format (the common case here: an
 * already-expanded message containing a stray '%') would walk the cursor onto stack
 * bytes that are not arguments at all. */
static bool next_arg(const kernel_call_frame *frame, unsigned *cursor, uint32_t *out)
{
    if (frame == NULL) {
        return false;
    }
    if (!kernel_frame_arg(frame, *cursor, out)) {
        return false;
    }
    (*cursor)++;
    return true;
}

size_t kernel_dbg_format(const kernel_call_frame *frame, kernel_guest_ptr format,
                         char *out, size_t out_size)
{
    size_t used = 0u;
    if (out == NULL || out_size == 0u) {
        return 0u;
    }
    out[0] = '\0';
    unsigned cursor = 1u; /* slot 0 is Format */

    for (uint32_t scanned = 0u; scanned < KERNEL_DBG_OUTPUT_MAX; scanned++) {
        uint8_t byte = 0u;
        if (!kernel_guest_read_u8(kernel_guest_add(format, scanned), &byte)) {
            /* The format ran off mapped memory. Say so in-line and stop: a marker in
             * the output is a diagnostic, a fault here would take the run down for a
             * debug print. */
            put_text(out, out_size, &used, "<format unreadable>");
            break;
        }
        if (byte == 0u) {
            break;
        }
        if (byte != (uint8_t)'%') {
            put_char(out, out_size, &used, (char)byte);
            continue;
        }

        /* A '%'. Read the specifier byte; a trailing lone '%' is emitted raw. */
        uint8_t spec = 0u;
        if (scanned + 1u >= KERNEL_DBG_OUTPUT_MAX
            || !kernel_guest_read_u8(kernel_guest_add(format, scanned + 1u), &spec)
            || spec == 0u) {
            put_char(out, out_size, &used, '%');
            continue;
        }

        char scratch[16];
        uint32_t value = 0u;
        switch (spec) {
        case '%':
            put_char(out, out_size, &used, '%');
            scanned++;
            break;
        case 'c':
            if (!next_arg(frame, &cursor, &value)) {
                put_text(out, out_size, &used, "<arg?>");
            } else {
                char printable = (char)(value & 0xFFu);
                put_char(out, out_size, &used,
                         (printable >= 0x20 && printable < 0x7F) ? printable : '.');
            }
            scanned++;
            break;
        case 'd':
        case 'i':
            if (!next_arg(frame, &cursor, &value)) {
                put_text(out, out_size, &used, "<arg?>");
            } else {
                (void)snprintf(scratch, sizeof(scratch), "%d", (int32_t)value);
                put_text(out, out_size, &used, scratch);
            }
            scanned++;
            break;
        case 'u':
            if (!next_arg(frame, &cursor, &value)) {
                put_text(out, out_size, &used, "<arg?>");
            } else {
                (void)snprintf(scratch, sizeof(scratch), "%u", value);
                put_text(out, out_size, &used, scratch);
            }
            scanned++;
            break;
        case 'x':
        case 'X':
            if (!next_arg(frame, &cursor, &value)) {
                put_text(out, out_size, &used, "<arg?>");
            } else {
                (void)snprintf(scratch, sizeof(scratch), spec == 'x' ? "%x" : "%X",
                               value);
                put_text(out, out_size, &used, scratch);
            }
            scanned++;
            break;
        case 'p':
            if (!next_arg(frame, &cursor, &value)) {
                put_text(out, out_size, &used, "<arg?>");
            } else {
                (void)snprintf(scratch, sizeof(scratch), "0x%08x", value);
                put_text(out, out_size, &used, scratch);
            }
            scanned++;
            break;
        case 's':
            if (!next_arg(frame, &cursor, &value)) {
                put_text(out, out_size, &used, "<arg?>");
            } else if (value == 0u) {
                put_text(out, out_size, &used, "(null)");
            } else {
                uint32_t offset = 0u;
                for (; offset < KERNEL_DBG_STRING_ARG_MAX; offset++) {
                    uint8_t sb = 0u;
                    if (!kernel_guest_read_u8(kernel_guest_add(value, offset), &sb)) {
                        /* Offset 0 unreadable means the POINTER is bad; later it is
                         * an unterminated string running off the mapping. Both get a
                         * marker, neither gets a fault. */
                        put_text(out, out_size, &used,
                                 offset == 0u ? "<bad %s ptr>" : "<unterminated>");
                        break;
                    }
                    if (sb == 0u) {
                        break;
                    }
                    put_char(out, out_size, &used, (char)sb);
                }
            }
            scanned++;
            break;
        default:
            /* RAW PASSTHROUGH, NO ARGUMENT CONSUMED. Width, flags and length syntax
             * ("%08x", "%-5d", "%ld") land here one byte at a time, as does any
             * stray '%' in an already-formatted message. Wrong-looking output is the
             * acceptable cost; a cursor walked onto non-argument stack bytes is not. */
            put_char(out, out_size, &used, '%');
            break;
        }
    }

    out[used] = '\0';
    return used;
}

static uint32_t hle_dbg_print(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t format = 0u;

    if (frame == NULL || !kernel_frame_arg(frame, 0u, &format)) {
        refused_count++;
        kernel_hle_log()("kernel: DbgPrint could not read its Format argument from "
                         "the guest stack -- nothing printed\n");
        return 0u;
    }
    if (format == 0u) {
        refused_count++;
        kernel_hle_log()("kernel: DbgPrint(Format=NULL) -- nothing printed\n");
        return 0u;
    }

    char message[KERNEL_DBG_OUTPUT_MAX];
    size_t length = kernel_dbg_format(frame, format, message, sizeof(message));
    /* The guest's own messages end in '\n' (they go to a kernel debugger console);
     * strip it so the sink's newline is the only one and the log stays one line per
     * print. */
    while (length > 0u && (message[length - 1u] == '\n' || message[length - 1u] == '\r')) {
        length--;
        message[length] = '\0';
    }
    print_count++;
    kernel_hle_log()("DbgPrint: %s\n", message);
    /* ULONG, and the real export answers STATUS_SUCCESS. Returned unconditionally:
     * nothing a debug print does should ever steer the caller. */
    return 0u;
}

/* ARITY-OK(95): ONE stack argument, BugCheckCode. Import slot 0x4759D0, measured row
 * {95, 1, 3 lifted sites, unanimous}, and the nxdk oracle KeBugCheck@4 agrees. Site
 * 0x003CB24B (sub_003CB1BA, recomp_0051.c ~148) pushes EBX, which is 0 on the only path
 * that reaches it, and a dead `ret` follows. Site 0x003CC9AA (sub_003CC97D,
 * recomp_0051.c ~5508, lifted twice more for overlapping entries) pushes the literal
 * 0xC0000144 with only an int3 slide byte after it. The full note is in kernel_dbg.h.
 *
 * Never continue semantics, the fatal call IS the implementation. The trailing return
 * of 0 exists for the capturing test hook alone. A frame that cannot supply the code
 * still goes fatal, naming the code as unreadable, rather than refusing quietly. */
static uint32_t hle_ke_bug_check(void *context)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t code = 0u;

    if (frame == NULL || !kernel_frame_arg(frame, 0u, &code)) {
        kernel_hle_fatal(KERNEL_DBG_ORD_KE_BUG_CHECK,
                         "KeBugCheck(<code unreadable>), the title halted itself");
        return 0u;
    }
    kernel_hle_fatal(KERNEL_DBG_ORD_KE_BUG_CHECK,
                     "KeBugCheck(0x%08X), the title halted itself", code);
    return 0u;
}

static const struct {
    unsigned ordinal;
    kernel_fn handler;
} bindings[] = {
    {KERNEL_DBG_ORD_DBG_PRINT, hle_dbg_print},
    {KERNEL_DBG_ORD_KE_BUG_CHECK, hle_ke_bug_check},
};

unsigned kernel_dbg_register(void)
{
    unsigned bound = 0u;
    for (size_t i = 0u; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        if (kernel_hle_register(bindings[i].ordinal, bindings[i].handler)) {
            bound++;
        }
    }
    return bound;
}

void kernel_dbg_reset(void)
{
    print_count = 0u;
    refused_count = 0u;
}

unsigned kernel_dbg_print_count(void)
{
    return print_count;
}

unsigned kernel_dbg_refused_count(void)
{
    return refused_count;
}
