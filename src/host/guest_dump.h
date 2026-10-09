/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1599: READ-ONLY dump of ranges of guest memory (--dump-guest-range), a census aid. The dump itself never writes
 * into the guest (T1613 adds the separate, opt-in guarded poke, guest_poke.h). The ranges are parsed and bounded by guest_dump_ranges.c (no guest dependency, so the option parser
 * can use it), the dump itself is guest_dump.c.
 */
#ifndef TSFP_HOST_GUEST_DUMP_H
#define TSFP_HOST_GUEST_DUMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GUEST_DUMP_MAX_RANGES 16u
/* The guest address space is 0..0x03FFFFFF (64 MiB), a range may not leave it or wrap. */
#define GUEST_DUMP_ADDRESS_LIMIT 0x04000000u
/* An indirect range's pointee may lie anywhere in the 32-bit guest space (heap pointers are above 0x04000000), it may
 * only not wrap past it. */
#define GUEST_DUMP_POINTEE_LIMIT 0x100000000ull
#define GUEST_DUMP_DEFAULT_MAX_BYTES 0x100000u /* total bytes of one dump, default 1 MiB */
#define GUEST_DUMP_HARD_MAX_BYTES 0x04000000u  /* --dump-guest-max-bytes may not raise it above the address space */

typedef struct {
    uint32_t address; /* direct: first byte dumped. indirect: address of the 32-bit pointer dword */
    uint32_t length;
    bool indirect;   /* `*ADDR+OFF:LEN`: dump LEN bytes at [ADDR] + offset, resolved at dump time */
    uint32_t offset; /* indirect only */
    bool twice;      /* T1759 `**ADDR+OFF1+OFF2:LEN`: [[ADDR]+offset]+offset2, indirect is true as well */
    uint32_t offset2;
} guest_dump_range;

typedef struct {
    guest_dump_range ranges[GUEST_DUMP_MAX_RANGES];
    unsigned count;
    uint64_t total_bytes;
} guest_dump_set;

/** Parse `ADDR:LEN` or the pointer-indirect `*ADDR[+OFF]:LEN` items and the two level `**ADDR+OFF1+OFF2:LEN` (T1759, both offsets required), comma separated (C integer syntax, base 0, no sign, no blanks) and append to `set`. Refuses an empty
 * list or item, a malformed or zero or oversize number, a start outside 0..0x03FFFFFF, a range that ends past
 * 0x04000000 (this includes wrapping 32-bit sums), and more than GUEST_DUMP_MAX_RANGES ranges. An indirect item needs
 * ADDR+4 inside the space and OFF+LEN at most 0x04000000 (no wrap), the pointee itself may be anywhere in 32 bits, `*` only as first character, `+OFF` optional (default 0).
 * `set` is unchanged on failure. `error` (may be NULL) receives the reason. */
bool guest_dump_parse_append(guest_dump_set *set, const char *text, char *error, size_t error_size);

/** T1759 two level ranges (`**ADDR+OFF1+OFF2:LEN`): from the first pointer `first` (read at ADDR) return NULL and the
 * address of the second pointer dword in `*slot` (= first + OFF1), else why it is unreadable (null, wraps).
 * Then from the second pointer `second` (read at *slot) return NULL and `*final` (= second + OFF2), else the reason
 * (null, wraps past 2^32). A null pointer at either level is refused by name, never dereferenced. */
const char *guest_dump_twice_slot(const guest_dump_range *range, uint32_t first, uint32_t *slot);
const char *guest_dump_twice_target(const guest_dump_range *range, uint32_t second, uint32_t *final);

/** Resolve an indirect range once the pointer dword `pointer` was read. Returns NULL and the dump address in
 * `*final`, or the reason it is unreadable (pointer+offset+length wraps past 2^32, whether the page is mapped is the read's call).
 * Pure 64-bit arithmetic, no wrap. */
const char *guest_dump_indirect_target(const guest_dump_range *range, uint32_t pointer, uint32_t *final);

/** True when the set is non-empty and its total fits `max_bytes` (itself at most GUEST_DUMP_HARD_MAX_BYTES). */
bool guest_dump_check_total(const guest_dump_set *set, uint64_t max_bytes);

/** Write one dump of `set` to `path` (via a temporary file and rename). Text, one hex line of 16 bytes per row, a range
 * that cannot be read is written as `unreadable` and makes the result false. Deterministic: no times in the file. */
bool guest_dump_write(const guest_dump_set *set, const char *path);

/** Start per-phase dumping: `dir` is created, a dump named `dir/guestdump.<phase>` is written on every SIGUSR1 (phase
 * = first word of the control file `dir/guestdump.phase`, else 1, 2, 3...), then `dir/guestdump.ack` is written
 * (`<dumps done> <phase>`), `dir/guestdump.pid` holds the host pid. An earlier SIGUSR1 handler is chained.
 * T1614: SIGUSR2 is the poke trigger: label = first word of `dir/guestpoke.phase` (else poke<n>), it serves
 * `dir/guestpoke.<label>`, writes `guestdump.<label>` and `dir/guestpoke.ack` (`<n> <label>`), and never counts as a
 * SIGUSR1 phase (the census and cpu sampler share SIGUSR1). An earlier SIGUSR2 handler is chained. */
bool guest_dump_start(const guest_dump_set *set, uint64_t max_bytes, const char *dir);

/** T1613: allow the guarded pokes (guest_poke.h) at the phase dumps, set BEFORE guest_dump_start. Default false: a
 * guestpoke.<label> request is then logged as REFUSED and nothing is written. `present` (may be NULL) gives the
 * present index recorded with each poke. */
void guest_dump_set_forced_state(bool forced_state, uint64_t (*present)(void));

/** T1616: serve DIR/guestpoke.<label> NOW, on the calling thread (the route replay calls it from the guest's pad poll,
 * so the poke lands at an exact route position), then write guestdump.<label> and guestpoke.ack. Same guards and
 * logging as the SIGUSR2 path (forced state, XBE image, whole-request validation). False when the dump was not started
 * or the label is empty; true otherwise (a missing request file is logged by the dump line, nothing is written). */
bool guest_dump_poke_now(const char *label);

/** Like guest_dump_write with `header` (may be NULL, lines starting with '#') inserted after the file's own header. */
bool guest_dump_write_with_header(const guest_dump_set *set, const char *path, const char *header);

/** T1629: a binary snapshot of a dump set. guest_dump_capture does the reads (read only, through
 * kernel_guest_read_bytes, no formatting, no file IO) on the calling thread, guest_dump_snapshot_write formats the SAME
 * text as guest_dump_write_with_header (one shared walk, tests/c/test_guest_dump.c compares the bytes) on any thread and
 * stores it atomically (tmp + rename). A range unreadable at capture is written as `unreadable`, the write returns
 * false then. NULL from capture only for an empty set or no memory. */
typedef struct guest_dump_snapshot guest_dump_snapshot;
guest_dump_snapshot *guest_dump_capture(const guest_dump_set *set);
bool guest_dump_snapshot_write(const guest_dump_snapshot *snapshot, const char *path, const char *header);
bool guest_dump_snapshot_complete(const guest_dump_snapshot *snapshot); /* every range read */
size_t guest_dump_snapshot_bytes(const guest_dump_snapshot *snapshot);  /* heap held by the snapshot */
void guest_dump_snapshot_free(guest_dump_snapshot *snapshot);
/** Upper bound of the text of one dump of `set` (about 3.6 bytes of text per guest byte), without extra header lines. */
uint64_t guest_dump_estimate_text_bytes(const guest_dump_set *set);

/** Stop the phase thread and write the final dump `dir/guestdump.exit`. Safe when never started. */
void guest_dump_stop(void);

#endif /* TSFP_HOST_GUEST_DUMP_H */
