/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1613 (step 2 of T1599): the GUARDED guest memory poke, a weapon census aid. FABRICATED-STATE: whatever a poked run
 * shows is not the original game's behaviour. It is never automatic: it needs the explicit host flag --forced-state
 * AND a request file DIR/guestpoke.<label> that the user triggers by sending the SIGUSR1 phase <label> (guest_dump.h).
 * T1614: the preferred trigger is SIGUSR2 (label from DIR/guestpoke.phase), which is not a census phase boundary.
 *
 * Request file, one poke per line (blank lines and '#' comments ignored, at most GUEST_POKE_MAX):
 *     ADDR[+OFF]=VALUE[:WIDTH]       direct: write at ADDR+OFF
 *     *ADDR[+OFF]=VALUE[:WIDTH]      indirect: the dword at ADDR is a pointer, write at [ADDR]+OFF
 * C integer syntax (base 0, no sign), VALUE must fit WIDTH, WIDTH is 1, 2 or 4 (default 4), the target is aligned to
 * its width. Guards: see guest_poke.c. The whole request is validated before the first byte is written, so a request
 * with one refused poke writes nothing.
 */
#ifndef TSFP_HOST_GUEST_POKE_H
#define TSFP_HOST_GUEST_POKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GUEST_POKE_MAX 16u
#define GUEST_POKE_MAX_BYTES 64u /* hard cap per poke (today the widths cap it at 4) */
#define GUEST_POKE_MAX_SECTIONS 64u
#define GUEST_POKE_LOW_LIMIT 0x10000u /* nothing below the XBE image base (null page, kernel scratch) */

typedef struct {
    bool indirect;
    uint32_t address; /* direct: base address. indirect: address of the pointer dword */
    uint32_t offset;
    unsigned width; /* 1, 2 or 4 */
    uint32_t value;
} guest_poke_item;

typedef struct {
    guest_poke_item items[GUEST_POKE_MAX];
    unsigned count;
} guest_poke_request;

/** XBE section flags (from the section header): writable 1, preload 2, executable 4. */
typedef struct {
    uint32_t address;
    uint32_t size;
    uint32_t flags;
    char name[12];
} guest_poke_section;

typedef struct {
    bool valid; /* false = header not readable or not an XBE: every poke is refused (fail closed) */
    uint32_t base;
    uint32_t size;
    unsigned count;
    guest_poke_section sections[GUEST_POKE_MAX_SECTIONS];
} guest_poke_image;

typedef struct {
    guest_poke_item item;
    uint32_t pointer; /* indirect: the pointer read */
    uint32_t target;
    uint32_t old_value;
    uint32_t read_back;
    bool written;
    char status[64]; /* "OK" or the refusal / failure reason */
} guest_poke_result;

typedef struct {
    guest_poke_result results[GUEST_POKE_MAX];
    unsigned count;
    unsigned written;
    bool ok; /* every poke written and read back */
    char summary[96];
} guest_poke_report;

/** Parse the request file text. false + `error` on any malformed line, an empty request or more than GUEST_POKE_MAX. */
bool guest_poke_parse(guest_poke_request *request, const char *text, char *error, size_t error_size);

/** Resolve one item to its target address. NULL on success (`*target` set), else the refusal reason. Pure arithmetic:
 * direct target+width must end inside 0..0x03FFFFFF, an indirect pointer dword must lie there too, the pointee plus
 * offset plus width may not wrap past 2^32, the target must be width aligned. `pointer` is ignored for direct items. */
const char *guest_poke_resolve(const guest_poke_item *item, uint32_t pointer, uint32_t *target);

/** Read the XBE section table from guest memory at 0x10000 into `image` (image->valid false on any problem). */
void guest_poke_load_image(guest_poke_image *image);

/** NULL when [target, target+width) may be written, else why not: below GUEST_POKE_LOW_LIMIT, any overlap of the XBE
 * image that is not entirely inside one writable non-executable section (header, .text, .rdata, read-only data), an
 * invalid image. Addresses past the image end (heap, stacks) are allowed. */
const char *guest_poke_check_target(const guest_poke_image *image, uint32_t target, unsigned width);

/** Test seam only: called between each write and its read-back (a test simulates a racing guest store). NULL in the host. */
extern void (*guest_poke_test_after_write)(uint32_t target);

/** Validate the whole request, then write it (kernel_guest_write_bytes), reading each target back. Without
 * `forced_state` nothing is touched. Returns report->ok. */
bool guest_poke_apply(const guest_poke_request *request, bool forced_state, const guest_poke_image *image,
                      guest_poke_report *report);

/** Text of a report, one line per poke, each line prefixed by `prefix`. */
void guest_poke_format(const guest_poke_report *report, const char *label, uint64_t present, const char *prefix,
                       char *out, size_t out_size);

/** Serve the SIGUSR1 phase `label`: when DIR/guestpoke.<label> exists, apply it (always logging, also a refusal),
 * append to DIR/guestpoke.log, write DIR/FORCED_STATE at the first written poke, rename the request to
 * guestpoke.<label>.done.<n> so it fires once, and put the report into `header` (for the dump header). Returns true
 * when a request file was found. */
bool guest_poke_consume(const char *dir, const char *label, bool forced_state, uint64_t present, char *header,
                        size_t header_size);

#endif /* TSFP_HOST_GUEST_POKE_H */
