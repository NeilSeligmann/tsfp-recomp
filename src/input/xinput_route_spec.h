/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_INPUT_XINPUT_ROUTE_SPEC_H
#define TSFP_INPUT_XINPUT_ROUTE_SPEC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* T1616: the command line grammars of the route replay (docs/input-replay.md, "Route replay").
 *
 * Wait spec (--route-wait):  markK:[mem=ADDR[:W]==VALUE[&MASK],][min=N,][max=N]
 *   The replay stalls (pad at rest, the recorded stream paused) at the position of recorded mark K until the guest
 *   memory condition holds and at least `min` polls were stalled. If `max` polls pass first the route FAILS (the host
 *   stops with the reason). ADDR/VALUE/MASK are C integers, W is 1, 2 or 4 (default 4). max defaults to
 *   XINPUT_ROUTE_DEFAULT_MAX_POLLS, a wait with neither mem nor min is refused (it would never wait).
 * Poke trigger spec (--poke-at-poll): WHERE:LABEL
 *   WHERE = N (the host's port 0 poll index, at or after), markK (replay reaches recorded mark K, after its wait),
 *   replay-end (the recorded stream is exhausted). LABEL = [A-Za-z0-9_-]{1,63}, the guestpoke.<LABEL> request. */
#define XINPUT_ROUTE_DEFAULT_MAX_POLLS 36000u
#define XINPUT_ROUTE_MAX_POLLS_LIMIT 10000000u
#define XINPUT_ROUTE_MAX_WAITS 8u
#define XINPUT_ROUTE_MAX_MARKS 16u
#define POKE_TRIGGER_MAX 4u

/* T1633: event conditions of an event wait (--route-wait-event, or a `# wait:` line of the record). The same grammar as
 * above (markK:field,field,...) with these fields, all of which must hold (AND) unless `any` is given:
 *   mem=[*]ADDR[+OFF][:W]OP VALUE[&MASK]  guest memory, OP in == != < <= > >=, `*` reads the pointer dword at ADDR first
 *   file-open=SUBSTR     a guest file whose path contains SUBSTR (case blind) was opened since the previous mark
 *                        (every SUBSTR below may start with `!`: the files that do NOT contain it, e.g. `!.pak`)
 *   file-read=SUBSTR[@BYTES]  that many bytes (default 1) were read from such files since the previous mark
 *   file-idle=MS[@SUBSTR]     file I/O happened since the previous mark and none for MS ms (SUBSTR: only such files)
 *   call=VA[@COUNT]      the lifted code called VA through the indirect call census COUNT times (default 1) since the previous mark
 *   frame-change         the presented frame fingerprint changed since the previous mark
 *   frame-stable=MS      ... and has not changed for MS ms
 *   min=N / min-ms=MS    stall at least that long (polls / wall ms) after the position of the mark was reached
 *   max=N / timeout=MS   the route FAILS after that many stalled polls / wall ms (default timeout 600000 ms)
 *   any                  the conditions are alternatives, min-* still gate
 * "Since the previous mark" is the run start (the first pad poll) for mark 1. */
#define ROUTE_COND_MAX 6u
#define ROUTE_COND_TEXT 48u
#define XINPUT_ROUTE_DEFAULT_TIMEOUT_MS 600000u
#define XINPUT_ROUTE_TIMEOUT_MS_LIMIT 86400000u

typedef enum {
    RCOND_MEM = 1,
    RCOND_FILE_OPEN,
    RCOND_FILE_READ,
    RCOND_FILE_IDLE,
    RCOND_CALL,
    RCOND_FRAME_CHANGE,
    RCOND_FRAME_STABLE,
} route_cond_kind;
typedef enum { RCMP_EQ = 1, RCMP_NE, RCMP_LT, RCMP_LE, RCMP_GT, RCMP_GE } route_cmp;

typedef struct {
    route_cond_kind kind;
    bool indirect;
    uint32_t address, offset;
    unsigned width;
    route_cmp cmp;
    uint32_t value, mask;
    char text[ROUTE_COND_TEXT]; /* file substring, lower case */
    uint32_t va;
    uint64_t amount; /* file-read bytes, file-idle / frame-stable ms, call count */
} route_cond;

typedef struct {
    unsigned mark; /* 1 based */
    bool has_mem;
    uint32_t address;
    unsigned width;
    uint32_t value, mask;
    uint64_t min_polls, max_polls; /* max_polls 0 = no poll ceiling (event waits only) */
    /* T1633 event waits */
    bool event;
    route_cond conds[ROUTE_COND_MAX];
    unsigned cond_count;
    bool any;
    uint64_t min_ms, timeout_ms;
} xinput_route_wait;

bool xinput_route_wait_parse(const char *spec, xinput_route_wait *out, char *error, size_t error_size);
/* T1633: the event grammar (a superset: the legacy fields also parse). */
bool xinput_route_event_wait_parse(const char *spec, xinput_route_wait *out, char *error, size_t error_size);
/* T1640: single memory conditions and their evaluation, shared by the route waits and the menu navigation (xinput_route_nav.h).
 * `read` is the guest memory reader (a null indirection pointer is unreadable). mem_ref_parse takes `[*]ADDR[+OFF][:W][&MASK]`, no
 * comparison (a cursor or count variable), mem_cond_parse the full `[*]ADDR[+OFF][:W]OP VALUE[&MASK]`. mem_address resolves the
 * address (indirect: the pointer at ADDR plus OFF, the base of a table), mem_read reads the W byte value there (unmasked). */
typedef bool (*xinput_route_read_fn)(uint32_t address, unsigned width, uint32_t *value, void *user);
bool xinput_route_mem_cond_parse(const char *text, route_cond *out);
bool xinput_route_mem_ref_parse(const char *text, route_cond *out);
bool xinput_route_mem_address(const route_cond *cond, xinput_route_read_fn read, void *user, uint32_t *address);
bool xinput_route_mem_read(const route_cond *cond, xinput_route_read_fn read, void *user, uint32_t *value);
bool xinput_route_compare(route_cmp cmp, uint32_t left, uint32_t right);
/* Short text of one condition for logs and failure messages ("file-open=anicemap.mkr"). */
size_t xinput_route_cond_format(const route_cond *cond, char *out, size_t out_size);

typedef enum { POKE_AT_POLL = 1, POKE_AT_MARK, POKE_AT_END } poke_where;
typedef struct {
    poke_where where;
    uint64_t value; /* poll index or mark number */
    char label[64];
    bool fired;
} poke_trigger_entry;

bool poke_trigger_parse(const char *spec, poke_trigger_entry *out, char *error, size_t error_size);

typedef struct {
    poke_trigger_entry entries[POKE_TRIGGER_MAX];
    unsigned count;
} poke_trigger_set;

/* The label of the first unfired entry that is due, marking it fired, else NULL. POLL: value (the poll index) >= the
 * entry's. MARK: value == the entry's mark number. END: any. Call until NULL. */
const char *poke_trigger_take(poke_trigger_set *set, poke_where where, uint64_t value);

#endif
