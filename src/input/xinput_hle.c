/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See xinput_hle.h for how the surface was found, why the site counts are exact here
 * and nowhere else, why every port defaults to EMPTY, and why no structure layout is
 * guessed.
 */

#include "xinput_hle.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>

static pthread_mutex_t state_mutex;
static pthread_once_t state_once = PTHREAD_ONCE_INIT;
static void state_mutex_init(void)
{
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) abort();
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0) abort();
    if (pthread_mutex_init(&state_mutex, &attr) != 0) abort();
    (void)pthread_mutexattr_destroy(&attr);
}
static void state_lock(void)
{ if (pthread_once(&state_once, state_mutex_init) != 0 || pthread_mutex_lock(&state_mutex) != 0) abort(); }
static void state_unlock(void)
{ if (pthread_mutex_unlock(&state_mutex) != 0) abort(); }

/* ===========================================================================
 * THE MEASURED SURFACE.
 *
 * Fifteen named code symbols in the `XPP` section of the retail executable. The
 * addresses and names come from `.XTLID`, resolved by `tools/xtlid.py`; the site
 * counts come from `tools/gen_d3d8_surface.py` scanning game `.text` for
 * `call rel32` / `jmp rel32` and bucketing targets by section.
 *
 * Declared in the generator's own order -- site count descending, address ascending
 * within a count -- so a diff against the generated file is a plain diff, with the two
 * zero-site rows appended because the generator does not emit them (see
 * XINPUT_FUNCTION_COUNT in the header for why they are here anyway).
 *
 * Not read from the generated table at build time ON PURPOSE: that table is derived
 * from the user's own executable and is gitignored, so depending on it would mean this
 * module and its tests could not build in a fresh clone.
 * `xinput_hle_crosscheck()` is the guard against the drift that duplication invites.
 *
 * ================== WHICH ROWS TO DOUBT, AND IN WHICH DIRECTION ==================
 *
 * Unusually for this tree, almost none. The header sets out the evidence: every row is
 * an `.XTLID` symbol start, the generator's entry-point test passes every row it
 * emitted image-wide with zero suspects, and an exhaustive every-alignment sweep of
 * the whole image found that no dword anywhere outside the fifteen `.XTLID` records
 * themselves holds any of these addresses -- so there is no indirect path into this
 * section to be blind to. The counts are exact.
 *
 * WHAT IS LEFT TO DOUBT IS NOT THE COUNTS BUT WHAT THEY IMPLY:
 *
 *   - `XInputGetState`, 1 site. The count is right and the RANKING it produces is
 *     badly wrong. One site, called every frame for every connected port, is the
 *     busiest function in this table by an enormous margin once anything actually
 *     runs. It ranks twelfth here. This single row is the reason
 *     `xinput_hle_report()` puts runtime count ahead of site count, and the reason a
 *     static backlog for this boundary should be read as "what exists" rather than
 *     "what to do first".
 *   - `XGetDevices`, 4 sites, and `XGetDeviceChanges`, 3. These top the static ranking
 *     and are genuinely multi-site, but they are enumeration calls: three of
 *     `XGetDevices`'s four sites sit in memory-unit code paths (0x000258CA, 0x0002FB08,
 *     0x0002FB14 all pass `XDEVICE_TYPE_MEMORY_UNIT_TABLE`), so most of what tops this
 *     table is save-game plumbing rather than the gamepad.
 *   - `XVoiceCreateMediaObjectEx`, 2 sites. Real, measured, and not input at all. It is
 *     in the table because it is in the section; it is labelled VOICE so that nobody
 *     spends a day on it thinking it is a controller function.
 *   - The two 0-site rows. `XReadMUMetaData` IS called -- once, from `XONLINE` -- and
 *     its zero records only that the generator counts `.text` origins. A zero here is a
 *     statement about the measurement's origin filter, not about the function.
 * =========================================================================== */
static const struct {
    uint32_t address;
    const char *name;
    uint32_t sites;
    xinput_fn_kind kind;
} XINPUT_SURFACE[XINPUT_FUNCTION_COUNT] = {
    {0x0046dbd2, "XGetDevices", 4, XINPUT_KIND_INPUT},
    {0x0046dbf4, "XGetDeviceChanges", 3, XINPUT_KIND_INPUT},
    {0x0046db91, "XPeekDevices", 2, XINPUT_KIND_INPUT},
    {0x0046e189, "XInputClose", 2, XINPUT_KIND_INPUT},
    {0x004754fd, "XVoiceCreateMediaObjectEx", 2, XINPUT_KIND_VOICE},
    {0x0046d7c4, "XMountMUA", 1, XINPUT_KIND_MEMORY_UNIT},
    {0x0046d8f6, "XUnmountMU", 1, XINPUT_KIND_MEMORY_UNIT},
    {0x0046dbcd, "XInitDevices", 1, XINPUT_KIND_INPUT},
    {0x0046e133, "XInputOpen", 1, XINPUT_KIND_INPUT},
    {0x0046e195, "XInputGetCapabilities", 1, XINPUT_KIND_INPUT},
    {0x0046e36d, "XInputGetState", 1, XINPUT_KIND_INPUT},
    {0x0046e3e0, "XInputSetState", 1, XINPUT_KIND_INPUT},
    {0x0046f3a8, "XGetDeviceEnumerationStatus", 1, XINPUT_KIND_INPUT},
    /* Measured at zero sites FROM `.text`. Both are real functions in the section. */
    {0x0046da04, "XReadMUMetaData", 0, XINPUT_KIND_MEMORY_UNIT},
    {0x004752c6, "XVoiceCreateMediaObject", 0, XINPUT_KIND_VOICE},
};

/* The six named DATA symbols in XPP. Addresses from `.XTLID`. Sorted by address,
 * because unlike the function table there is no site count to order them by. */
static const xinput_data_symbol XINPUT_DATA_SYMBOLS[XINPUT_DATA_SYMBOL_COUNT] = {
    {0x0046c6e0, "XDEVICE_TYPE_MEMORY_UNIT_TABLE"},
    {0x0046c75c, "XDEVICE_TYPE_GAMEPAD_TABLE"},
    {0x0046c7c0, "XDEVICE_TYPE_IR_REMOTE_TABLE"},
    {0x0046c894, "XDEVICE_TYPE_VOICE_MICROPHONE_TABLE"},
    {0x0046c8a0, "XDEVICE_TYPE_VOICE_HEADPHONE_TABLE"},
    {0x0046c8ac, "XDEVICE_TYPE_HIGHFIDELITY_MICROPHONE_TABLE"},
};

static xinput_entry entries[XINPUT_FUNCTION_COUNT];
static bool initialised;

static xinput_port_state ports[XINPUT_PORT_COUNT];
static uint64_t empty_queries;
static bool empty_reported;

static xinput_pad_state pads[XINPUT_PORT_COUNT];

static uint64_t synthetic_values;
static uint32_t report_changes[XINPUT_PORT_COUNT];
static bool synthetic_values_announced;

static uint32_t state_size;
static xinput_field_placement placements[XINPUT_FIELD_COUNT];
static bool measured_layout_adopted;
static xinput_write_fn guest_writer;
static void *guest_writer_user;
static uint64_t write_refusals;
static bool layout_unset_reported;

static uint64_t unknown_calls;

static int default_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vfprintf(stderr, format, args);
    va_end(args);
    return written;
}

static xinput_log_fn log_printer = default_printer;

void xinput_hle_set_log(xinput_log_fn printer)
{
    log_printer = printer ? printer : default_printer;
}

xinput_log_fn xinput_hle_log(void)
{
    return log_printer;
}

const char *xinput_hle_kind_name(xinput_fn_kind kind)
{
    switch (kind) {
    case XINPUT_KIND_INPUT:
        return "input";
    case XINPUT_KIND_MEMORY_UNIT:
        return "memory-unit";
    case XINPUT_KIND_VOICE:
        return "voice";
    }
    /* Not reachable through the public API, and deliberately not an abort: a
     * diagnostic helper that can kill the process is a liability in exactly the
     * situation it exists to explain. */
    return "unclassified";
}

const char *xinput_hle_entry_label(const xinput_entry *entry, char *buffer, size_t len)
{
    if (!entry) {
        return "<null entry>";
    }
    if (entry->name) {
        return entry->name;
    }
    /* "By address otherwise" is the required behaviour, not a fallback to a constant
     * placeholder: an unnamed row that logs as "<unnamed>" is indistinguishable from
     * every other unnamed row, which makes the log useless at precisely the point it
     * is needed most. All 15 rows are named in this image, so this branch exists for
     * a table regenerated from an image where some are not. */
    if (!buffer || len == 0) {
        return "<unnamed, no buffer>";
    }
    (void)snprintf(buffer, len, "<unnamed in .XTLID @ %#010x>", entry->address);
    return buffer;
}

static void locked_xinput_hle_init(void)
{
    memset(entries, 0, sizeof(entries));
    for (size_t i = 0; i < XINPUT_FUNCTION_COUNT; i++) {
        entries[i].address = XINPUT_SURFACE[i].address;
        entries[i].name = XINPUT_SURFACE[i].name;
        entries[i].sites = XINPUT_SURFACE[i].sites;
        entries[i].kind = XINPUT_SURFACE[i].kind;
        entries[i].state = XINPUT_ENTRY_STUB;
    }
    initialised = true;

    for (size_t i = 0; i < XINPUT_PORT_COUNT; i++) {
        ports[i] = XINPUT_PORT_EMPTY;
    }
    empty_queries = 0;
    empty_reported = false;

    memset(pads, 0, sizeof(pads));
    synthetic_values = 0;
    memset(report_changes, 0, sizeof(report_changes));
    synthetic_values_announced = false;

    measured_layout_adopted = false;
    state_size = XINPUT_STATE_SIZE_UNSET;
    for (size_t i = 0; i < XINPUT_FIELD_COUNT; i++) {
        placements[i].offset = XINPUT_OFFSET_UNSET;
        placements[i].width = 0;
        placements[i].mapped = false;
    }
    guest_writer = NULL;
    guest_writer_user = NULL;
    write_refusals = 0;
    layout_unset_reported = false;

    unknown_calls = 0;
}

static void ensure_initialised(void)
{
    if (!initialised) {
        xinput_hle_init();
    }
}

/*
 * EXACT-MATCH LOOKUP, LINEAR ON PURPOSE.
 *
 * Fifteen rows, and the table is in site-count order rather than address order, so a
 * binary search would need a second sorted index to exist at all. A linear scan over
 * 15 entries costs nothing next to the guest work around it, and it cannot silently
 * return a NEIGHBOUR the way an off-by-one bound in a binary search can -- which
 * matters here for a concrete reason: `XInitDevices` at 0x0046DBCD and `XGetDevices`
 * at 0x0046DBD2 are FIVE BYTES APART, and `XInputClose` at 0x0046E189 and
 * `XInputGetCapabilities` at 0x0046E195 are twelve. Dispatching one-time device
 * initialisation as a per-frame enumeration, or a handle close as a capability query,
 * is far worse than dispatching nothing. The suite tests every in-between byte.
 */
static xinput_entry *mutable_entry(uint32_t address)
{
    ensure_initialised();
    for (size_t i = 0; i < XINPUT_FUNCTION_COUNT; i++) {
        if (entries[i].address == address) {
            return &entries[i];
        }
    }
    return NULL;
}

bool xinput_hle_register(uint32_t address, xinput_fn handler)
{
    xinput_entry *entry = mutable_entry(address);
    if (!entry || !handler) {
        return false;
    }
    entry->handler = handler;
    entry->state = XINPUT_ENTRY_IMPLEMENTED;
    return true;
}

bool xinput_hle_set_default_return(uint32_t address, uint32_t value)
{
    xinput_entry *entry = mutable_entry(address);
    if (!entry) {
        return false;
    }
    entry->default_return = value;
    return true;
}

const xinput_entry *xinput_hle_entry(uint32_t address)
{
    return mutable_entry(address);
}

const xinput_entry *xinput_hle_entry_by_name(const char *name)
{
    ensure_initialised();
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < XINPUT_FUNCTION_COUNT; i++) {
        if (entries[i].name && strcmp(entries[i].name, name) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

const xinput_entry *xinput_hle_table(size_t *out_count)
{
    ensure_initialised();
    if (out_count) {
        *out_count = XINPUT_FUNCTION_COUNT;
    }
    return entries;
}

const xinput_data_symbol *xinput_hle_data_symbols(size_t *out_count)
{
    if (out_count) {
        *out_count = XINPUT_DATA_SYMBOL_COUNT;
    }
    return XINPUT_DATA_SYMBOLS;
}

const xinput_data_symbol *xinput_hle_data_symbol(uint32_t address)
{
    for (size_t i = 0; i < XINPUT_DATA_SYMBOL_COUNT; i++) {
        if (XINPUT_DATA_SYMBOLS[i].address == address) {
            return &XINPUT_DATA_SYMBOLS[i];
        }
    }
    return NULL;
}

uint32_t xinput_hle_call(uint32_t address, void *context)
{
    xinput_entry *entry = mutable_entry(address);
    if (!entry) {
        unknown_calls++;
        /* NOT a missing implementation. Our surface table and the binary disagree,
         * which is a worse and differently-shaped problem, so it reports EVERY time
         * rather than once. Three distinct messages, because they point at three
         * different first moves. */
        const xinput_data_symbol *data = xinput_hle_data_symbol(address);
        if (data) {
            /* The most specific and most useful of the three. A call to a device-type
             * descriptor table is an ARGUMENT that reached a call slot -- the tables
             * are passed by address to the enumeration and open calls, so a lifted
             * wrapper that confuses the two produces exactly this. Saying "unknown
             * target" here would send someone hunting for a missing function that
             * never existed. */
            log_printer("xinput: call to %#010x, which is the DATA symbol %s -- that is "
                        "a device-type descriptor table, not a function; an argument "
                        "has reached a call target\n",
                        address, data->name);
        } else if (address >= XINPUT_SECTION_VA_BEGIN && address < XINPUT_SECTION_VA_END) {
            log_printer("xinput: call to UNKNOWN target %#010x -- inside the XPP "
                        "section but absent from our %u-entry surface table\n",
                        address, (unsigned)XINPUT_FUNCTION_COUNT);
        } else {
            log_printer("xinput: call to UNKNOWN target %#010x -- OUTSIDE the XPP "
                        "section (%#010x..%#010x); this is not a peripheral address\n",
                        address, (unsigned)XINPUT_SECTION_VA_BEGIN,
                        (unsigned)XINPUT_SECTION_VA_END);
        }
        return 0;
    }

    entry->call_count++;

    if (entry->state == XINPUT_ENTRY_IMPLEMENTED && entry->handler) {
        return entry->handler(context);
    }

    if (!entry->reported) {
        entry->reported = true;
        /* Once per function. `XInputGetState` is polled every frame for every port;
         * per-call reporting would bury the one-shot `XInitDevices` and `XInputOpen`
         * calls, and that enumeration order is the single thing this module exists to
         * learn. The measured site count and the kind ride along so the log is
         * readable without the table next to it. */
        char label[64];
        log_printer("xinput: %#010x %s [%s] is not implemented (%u measured call "
                    "site%s), returning %#x\n",
                    entry->address, xinput_hle_entry_label(entry, label, sizeof(label)),
                    xinput_hle_kind_name(entry->kind), entry->sites,
                    entry->sites == 1u ? "" : "s", entry->default_return);
    }
    return entry->default_return;
}

size_t xinput_hle_implemented_count(void)
{
    ensure_initialised();
    size_t done = 0;
    for (size_t i = 0; i < XINPUT_FUNCTION_COUNT; i++) {
        if (entries[i].state == XINPUT_ENTRY_IMPLEMENTED) {
            done++;
        }
    }
    return done;
}

size_t xinput_hle_touched_count(void)
{
    ensure_initialised();
    size_t touched = 0;
    for (size_t i = 0; i < XINPUT_FUNCTION_COUNT; i++) {
        if (entries[i].call_count > 0) {
            touched++;
        }
    }
    return touched;
}

uint64_t xinput_hle_unknown_call_count(void)
{
    ensure_initialised();
    return unknown_calls;
}

/* ===================== PORTS ===================== */

static bool locked_xinput_hle_attach_synthetic_pad(unsigned port)
{
    ensure_initialised();
    if (port >= XINPUT_PORT_COUNT) {
        return false;
    }
    if (ports[port] == XINPUT_PORT_SYNTHETIC) {
        return true;
    }
    ports[port] = XINPUT_PORT_SYNTHETIC;
    /* A new device: at rest, no report counted (T731). First attach finds both already zero. */
    memset(&pads[port], 0, sizeof(pads[port]));
    report_changes[port] = 0u;
    /* FABRICATED, in the same voice as --stub-status, --mount, the config zero-fill
     * and DirectSound's forced codec readiness. Presence is the first bit the title
     * reads about input and it decides which branch the whole init takes, so a run log
     * that does not record it cannot be interpreted at all. */
    log_printer("xinput: port %u now reports a SYNTHETIC pad (FABRICATED -- there is "
                "no gamepad, no USB host controller and no host input backend; this "
                "fakes PRESENCE ONLY, and every button and axis stays at rest)\n",
                port);
    return true;
}

static bool locked_xinput_hle_detach_synthetic_pad(unsigned port)
{
    ensure_initialised();
    if (port >= XINPUT_PORT_COUNT) {
        return false;
    }
    if (ports[port] == XINPUT_PORT_EMPTY) {
        return true;
    }
    ports[port] = XINPUT_PORT_EMPTY;
    memset(&pads[port], 0, sizeof(pads[port]));
    log_printer("xinput: port %u is EMPTY again (the honest default)\n", port);
    return true;
}

static xinput_port_state locked_xinput_hle_port_state(unsigned port)
{
    ensure_initialised();
    if (port >= XINPUT_PORT_COUNT) {
        return XINPUT_PORT_EMPTY;
    }
    return ports[port];
}

static bool locked_xinput_hle_port_connected(unsigned port)
{
    ensure_initialised();
    if (port < XINPUT_PORT_COUNT && ports[port] == XINPUT_PORT_SYNTHETIC) {
        return true;
    }
    /* An out-of-range port counts as an EMPTY answer rather than being silently
     * dropped. A caller asking about port 7 is a bug worth seeing in the count. */
    empty_queries++;
    if (!empty_reported) {
        empty_reported = true;
        /* Once, not per query -- this is polled once per frame per port. The honest
         * default has a cost, so the cost is signposted here rather than left for
         * someone to rediscover as "the menu will not respond". */
        log_printer("xinput: no device on any port -- NOTHING IS READ FROM HARDWARE, so "
                    "enumeration returns nothing by construction. Expect a title that "
                    "boots and then cannot be driven: a start screen that never "
                    "advances, or a 'please reconnect the controller' modal that never "
                    "clears. Attach the fabricated synthetic pad if you want past "
                    "this.\n");
    }
    return false;
}

static unsigned locked_xinput_hle_connected_count(void)
{
    ensure_initialised();
    unsigned count = 0;
    for (unsigned i = 0; i < XINPUT_PORT_COUNT; i++) {
        if (ports[i] == XINPUT_PORT_SYNTHETIC) {
            count++;
        }
    }
    return count;
}

uint64_t xinput_hle_empty_query_count(void)
{
    ensure_initialised();
    return empty_queries;
}

/* ===================== LAYOUT ===================== */

const char *xinput_hle_field_name(xinput_field field)
{
    switch (field) {
    case XINPUT_FIELD_PACKET_NUMBER:
        return "packet_number";
    case XINPUT_FIELD_DIGITAL_BUTTONS:
        return "digital_buttons";
    case XINPUT_FIELD_ANALOG_RUN:
        return "analog_run";
    case XINPUT_FIELD_THUMB_LEFT_X:
        return "thumb_left_x";
    case XINPUT_FIELD_THUMB_LEFT_Y:
        return "thumb_left_y";
    case XINPUT_FIELD_THUMB_RIGHT_X:
        return "thumb_right_x";
    case XINPUT_FIELD_THUMB_RIGHT_Y:
        return "thumb_right_y";
    case XINPUT_FIELD_COUNT:
        break;
    }
    return NULL;
}

void xinput_hle_set_state_size(uint32_t size)
{
    ensure_initialised();
    state_size = size;
    /* Any size change makes this no longer the adopted measured layout, even if the
     * caller is in the middle of re-adopting it: `adopt` sets the flag only once every
     * field has passed validation, so a half-applied adoption never reports itself as
     * the measured one. */
    measured_layout_adopted = false;
    /* EVERY size change clears EVERY mapping, including a change to a larger size.
     * A field was accepted because it fitted inside the size in force at the time; if
     * that size is no longer in force then the field is simply unvalidated, and an
     * unvalidated offset is the `IO_STATUS_BLOCK` overrun with one extra step. Keeping
     * mappings across a shrink would reintroduce it directly. */
    for (size_t i = 0; i < XINPUT_FIELD_COUNT; i++) {
        placements[i].offset = XINPUT_OFFSET_UNSET;
        placements[i].width = 0;
        placements[i].mapped = false;
    }
    if (size == XINPUT_STATE_SIZE_UNSET) {
        log_printer("xinput: guest state size cleared; every field mapping dropped\n");
    } else {
        log_printer("xinput: guest state size set to %u bytes (DERIVED); every field "
                    "mapping dropped and must be re-supplied against this size\n",
                    size);
    }
}

uint32_t xinput_hle_state_size(void)
{
    ensure_initialised();
    return state_size;
}

bool xinput_hle_map_field(xinput_field field, uint32_t offset, uint32_t width)
{
    ensure_initialised();
    if (field >= XINPUT_FIELD_COUNT) {
        log_printer("xinput: map_field refused -- field id %d is out of range\n",
                    (int)field);
        return false;
    }
    const char *name = xinput_hle_field_name(field);
    if (state_size == XINPUT_STATE_SIZE_UNSET) {
        log_printer("xinput: map_field(%s) refused -- no guest state size has been "
                    "derived, so there is nothing to bound the field against\n",
                    name);
        return false;
    }
    if (width == 0) {
        log_printer("xinput: map_field(%s) refused -- a width of 0 writes nothing and "
                    "is not a derivation\n",
                    name);
        return false;
    }
    /* A scalar field has to be a scalar size. The analog region is a RUN of per-entry
     * pressures rather than one number, so it is the one field allowed any width -- and
     * its length is itself a measurement, eight from the table at 0x0047DF40. */
    if (field != XINPUT_FIELD_ANALOG_RUN && width != 1u && width != 2u &&
        width != 4u) {
        log_printer("xinput: map_field(%s) refused -- width %u is not a scalar access "
                    "width; derive it from the access width at the call site\n",
                    name, width);
        return false;
    }
    /* THE `IO_STATUS_BLOCK` CHECK. A wrapping sum and a sum past the end are the same
     * bug, and both are rejected rather than clamped: clamping would write a truncated
     * field at a wrong offset and report success. */
    if (offset > UINT32_MAX - width || offset + width > state_size) {
        log_printer("xinput: map_field(%s) refused -- offset %#x + width %u exceeds the "
                    "derived state size of %u bytes. This is the IO_STATUS_BLOCK "
                    "failure exactly: a layout from the wrong source overruns every "
                    "structure it touches.\n",
                    name, offset, width, state_size);
        return false;
    }
    for (size_t i = 0; i < XINPUT_FIELD_COUNT; i++) {
        if (i == (size_t)field || !placements[i].mapped) {
            continue;
        }
        uint32_t other_begin = placements[i].offset;
        uint32_t other_end = other_begin + placements[i].width;
        if (offset < other_end && other_begin < offset + width) {
            /* Two fields claiming the same bytes means at least one derivation is
             * wrong. Accepting both would write one over the other in an order nobody
             * chose, and the symptom would be a field that is intermittently right. */
            log_printer("xinput: map_field(%s) refused -- bytes %#x..%#x overlap %s at "
                        "%#x..%#x; at least one of the two derivations is wrong\n",
                        name, offset, offset + width,
                        xinput_hle_field_name((xinput_field)i), other_begin, other_end);
            return false;
        }
    }
    placements[field].offset = offset;
    placements[field].width = width;
    placements[field].mapped = true;
    /* A hand-mapped field means this is no longer purely the compiled-in measured
     * layout, so it stops claiming to be. `adopt` re-sets the flag after its last field
     * passes, which is why it can call through here without defeating itself. */
    measured_layout_adopted = false;
    log_printer("xinput: %s mapped to guest offset %#x, width %u (DERIVED)\n", name,
                offset, width);
    return true;
}

xinput_field_placement xinput_hle_field(xinput_field field)
{
    ensure_initialised();
    if (field >= XINPUT_FIELD_COUNT) {
        xinput_field_placement none = {XINPUT_OFFSET_UNSET, 0, false};
        return none;
    }
    return placements[field];
}

unsigned xinput_hle_mapped_field_count(void)
{
    ensure_initialised();
    unsigned count = 0;
    for (size_t i = 0; i < XINPUT_FIELD_COUNT; i++) {
        if (placements[i].mapped) {
            count++;
        }
    }
    return count;
}

bool xinput_hle_layout_usable(void)
{
    ensure_initialised();
    return state_size != XINPUT_STATE_SIZE_UNSET && xinput_hle_mapped_field_count() > 0;
}

static xinput_pad_state locked_xinput_hle_pad_state(unsigned port)
{
    ensure_initialised();
    if (port >= XINPUT_PORT_COUNT) {
        xinput_pad_state zero;
        memset(&zero, 0, sizeof(zero));
        return zero;
    }
    return pads[port];
}

void xinput_hle_set_writer(xinput_write_fn writer, void *user)
{
    ensure_initialised();
    guest_writer = writer;
    guest_writer_user = user;
}

/*
 * The host-side value of a scalar field, widened to 32 bits.
 *
 * Signed stick axes are converted to their two's-complement bit pattern first, so that
 * narrowing to a derived width is a plain byte truncation rather than a sign question.
 */
static uint32_t scalar_value(const xinput_pad_state *pad, xinput_field field)
{
    switch (field) {
    case XINPUT_FIELD_PACKET_NUMBER:
        return pad->packet_number;
    case XINPUT_FIELD_DIGITAL_BUTTONS:
        return pad->digital_buttons;
    case XINPUT_FIELD_THUMB_LEFT_X:
        return (uint32_t)(uint16_t)pad->thumb_left_x;
    case XINPUT_FIELD_THUMB_LEFT_Y:
        return (uint32_t)(uint16_t)pad->thumb_left_y;
    case XINPUT_FIELD_THUMB_RIGHT_X:
        return (uint32_t)(uint16_t)pad->thumb_right_x;
    case XINPUT_FIELD_THUMB_RIGHT_Y:
        return (uint32_t)(uint16_t)pad->thumb_right_y;
    case XINPUT_FIELD_ANALOG_RUN:
    case XINPUT_FIELD_COUNT:
        break;
    }
    return 0;
}

bool xinput_hle_adopt_measured_layout(void)
{
    ensure_initialised();

    /* Set the size first; it clears every existing mapping, which is what makes
     * adoption a wholesale replacement rather than a merge with whatever was there. */
    xinput_hle_set_state_size(XINPUT_MEASURED_STATE_SIZE);

    static const struct {
        xinput_field field;
        uint32_t offset;
        uint32_t width;
    } MEASURED[] = {
        {XINPUT_FIELD_PACKET_NUMBER, XINPUT_MEASURED_OFFSET_PACKET_NUMBER, 4u},
        {XINPUT_FIELD_DIGITAL_BUTTONS, XINPUT_MEASURED_OFFSET_DIGITAL_BUTTONS, 2u},
        {XINPUT_FIELD_ANALOG_RUN, XINPUT_MEASURED_OFFSET_ANALOG_RUN, XINPUT_ANALOG_COUNT},
        {XINPUT_FIELD_THUMB_LEFT_X, XINPUT_MEASURED_OFFSET_THUMB_LEFT_X, 2u},
        {XINPUT_FIELD_THUMB_LEFT_Y, XINPUT_MEASURED_OFFSET_THUMB_LEFT_Y, 2u},
        {XINPUT_FIELD_THUMB_RIGHT_X, XINPUT_MEASURED_OFFSET_THUMB_RIGHT_X, 2u},
        {XINPUT_FIELD_THUMB_RIGHT_Y, XINPUT_MEASURED_OFFSET_THUMB_RIGHT_Y, 2u},
    };

    for (size_t i = 0; i < sizeof(MEASURED) / sizeof(MEASURED[0]); i++) {
        /* Through the SAME validation a hand-derived mapping goes through, on purpose.
         * The compiled-in table being measured does not exempt it from the bound and
         * overlap checks -- a measured table that overruns its own declared size is the
         * exact failure this module exists to prevent, and letting it skip the gate
         * because of where it came from would be the one unforgivable shortcut here. */
        if (!xinput_hle_map_field(MEASURED[i].field, MEASURED[i].offset,
                                  MEASURED[i].width)) {
            log_printer("xinput: adopting the measured layout FAILED on %s -- the "
                        "compiled-in table does not pass its own validation, which is a "
                        "bug in xinput_hle.c and not in the caller\n",
                        xinput_hle_field_name(MEASURED[i].field));
            xinput_hle_set_state_size(XINPUT_STATE_SIZE_UNSET);
            return false;
        }
    }

    measured_layout_adopted = true;
    log_printer("xinput: adopted the MEASURED guest state layout -- %u bytes, %d fields, "
                "derived from the retail image (buffer from `lea ecx,[esp+0x10]` at "
                "0x0018FFA5; analog run is EIGHT bytes at 0x06, six face buttons plus "
                "two triggers, from the 8-entry table at 0x0047DF40). The frame the "
                "title allocates is %u; the last %u bytes are excluded because whether "
                "they are padding is NOT decidable from this image.\n",
                (unsigned)XINPUT_MEASURED_STATE_SIZE, (int)XINPUT_FIELD_COUNT,
                (unsigned)XINPUT_MEASURED_STATE_FRAME,
                (unsigned)(XINPUT_MEASURED_STATE_FRAME - XINPUT_MEASURED_STATE_SIZE));
    return true;
}

bool xinput_hle_measured_layout_adopted(void)
{
    ensure_initialised();
    return measured_layout_adopted;
}

static bool locked_xinput_hle_set_synthetic_pad_state(unsigned port, xinput_pad_state state)
{
    ensure_initialised();
    if (port >= XINPUT_PORT_COUNT) {
        return false;
    }
    if (ports[port] != XINPUT_PORT_SYNTHETIC) {
        /* Refused rather than silently honoured. Values for a port the guest has been
         * told is empty would be input from a controller that does not exist even by
         * this module's own account, and that inconsistency must surface here rather
         * than as a mysterious button press later. */
        log_printer("xinput: synthetic values for port %u REFUSED -- that port reports "
                    "EMPTY, so there is no device for the values to belong to. Attach "
                    "the synthetic pad first.\n",
                    port);
        return false;
    }
    /* T731: the original counts every raw report change, so count them here, not between the title's polls. */
    xinput_pad_state before = pads[port], after = state;
    before.packet_number = after.packet_number = 0u;
    if (memcmp(&before, &after, sizeof(before)) != 0) report_changes[port]++;
    pads[port] = state;
    synthetic_values++;
    if (!synthetic_values_announced) {
        synthetic_values_announced = true;
        /* A separate and louder banner than mere presence, because this is the bigger
         * lie: the guest will ACT on these. */
        log_printer("xinput: port %u given SYNTHETIC BUTTON AND AXIS VALUES (FABRICATED "
                    "-- nothing is read from hardware; these came from outside this "
                    "module and the guest will act on them as though a player had moved "
                    "a real controller)\n",
                    port);
    }
    return true;
}

static uint32_t locked_xinput_hle_synthetic_report_changes(unsigned port)
{
    ensure_initialised();
    return port < XINPUT_PORT_COUNT ? report_changes[port] : 0u;
}

uint64_t xinput_hle_synthetic_value_count(void)
{
    ensure_initialised();
    return synthetic_values;
}

static unsigned locked_xinput_hle_write_guest_state(unsigned port, uint32_t guest_address)
{
    ensure_initialised();

    if (!xinput_hle_layout_usable()) {
        write_refusals++;
        if (!layout_unset_reported) {
            layout_unset_reported = true;
            /* Refuse rather than fall back to a familiar-looking layout. This is the
             * single most important refusal in the module. */
            log_printer("xinput: guest state write REFUSED -- no layout has been "
                        "derived for this image (size %s, %u of %d fields mapped). "
                        "Nothing is written and nothing is guessed: a lifter patch "
                        "found upstream declared IO_STATUS_BLOCK at 16 bytes where the "
                        "guest's is 8 and overran every one by 8. The desktop XInput "
                        "layout is NOT the answer either -- this pad's six "
                        "pressure-sensitive face buttons have no desktop counterpart, "
                        "so every offset after them diverges. Derive offsets and widths "
                        "from the accesses at the call sites, per "
                        "docs/guest-structs.md.\n",
                        state_size == XINPUT_STATE_SIZE_UNSET ? "UNSET" : "set",
                        xinput_hle_mapped_field_count(), (int)XINPUT_FIELD_COUNT);
        }
        return 0;
    }

    if (!guest_writer) {
        write_refusals++;
        log_printer("xinput: guest state write at %#010x has no writer installed; "
                    "nothing was written\n",
                    guest_address);
        return 0;
    }

    if (port >= XINPUT_PORT_COUNT) {
        write_refusals++;
        log_printer("xinput: guest state write refused -- port %u does not exist\n",
                    port);
        return 0;
    }

    const xinput_pad_state *pad = &pads[port];
    unsigned written = 0;

    for (size_t i = 0; i < XINPUT_FIELD_COUNT; i++) {
        if (!placements[i].mapped) {
            /* Skipped, not guessed. A partial layout is worth using for the fields it
             * covers; inventing the rest is the whole failure this module refuses. */
            continue;
        }
        uint32_t width = placements[i].width;
        uint8_t bytes[256];
        if (width > sizeof(bytes)) {
            /* Cannot happen while the state size is sane, and a silent buffer overrun
             * here would be a fine irony in the module whose point is not overrunning
             * guest structures. */
            log_printer("xinput: %s width %u exceeds the marshalling buffer; skipped\n",
                        xinput_hle_field_name((xinput_field)i), width);
            continue;
        }
        if ((xinput_field)i == XINPUT_FIELD_ANALOG_RUN) {
            /* Entry for entry, no rearrangement: the host array mirrors the guest run,
             * triggers included at entries 6 and 7. A derived width shorter than eight
             * writes a prefix; a longer one zero-fills rather than reading past the host
             * array, which would be an out-of-bounds read in the module whose entire
             * point is not overrunning things. */
            for (uint32_t b = 0; b < width; b++) {
                bytes[b] = b < XINPUT_ANALOG_COUNT ? pad->analog[b] : 0u;
            }
        } else {
            uint32_t value = scalar_value(pad, (xinput_field)i);
            /* Little-endian explicitly, rather than memcpy of a host integer. The guest
             * is little-endian; writing host byte order would be right by accident on
             * this build host and wrong on another, which is the kind of bug that only
             * shows up on someone else's machine. */
            for (uint32_t b = 0; b < width; b++) {
                bytes[b] = (uint8_t)((value >> (8u * b)) & 0xFFu);
            }
        }
        guest_writer(guest_address + placements[i].offset, bytes, width,
                     guest_writer_user);
        written++;
    }

    return written;
}

uint64_t xinput_hle_write_refused_count(void)
{
    ensure_initialised();
    return write_refusals;
}

/* ===================== REPORT ===================== */

/*
 * Is `candidate` a better place in the report than `best`?
 *
 * Runtime calls, then measured sites, then address ascending for determinism. Split
 * out as its own function because the ordering IS the product here: a report sorted
 * the wrong way round is not a cosmetic defect, it is a work queue that points at the
 * least important function first.
 */
static bool ranks_above(const xinput_entry *candidate, const xinput_entry *best)
{
    if (candidate->call_count != best->call_count) {
        return candidate->call_count > best->call_count;
    }
    if (candidate->sites != best->sites) {
        return candidate->sites > best->sites;
    }
    return candidate->address < best->address;
}

void xinput_hle_report(void)
{
    ensure_initialised();

    size_t missing = 0;
    uint64_t observed = 0;
    for (size_t i = 0; i < XINPUT_FUNCTION_COUNT; i++) {
        if (entries[i].state != XINPUT_ENTRY_IMPLEMENTED) {
            missing++;
        }
        observed += entries[i].call_count;
    }

    log_printer("xinput: %zu of %u measured functions still need implementations "
                "(%u call sites measured, %llu calls observed this run)\n",
                missing, (unsigned)XINPUT_FUNCTION_COUNT, (unsigned)XINPUT_SITE_COUNT,
                (unsigned long long)observed);

    /* SAY WHAT THE INPUT ACTUALLY DID. A reader of a run log must not be able to
     * mistake this for a gamepad that failed to open: nothing is polled and nothing is
     * read, by construction and not by accident. */
    log_printer("xinput: input is READ-FROM-NOTHING -- no host input device is opened, "
                "no USB bus is enumerated and not one byte is read from any host input "
                "API\n");

    unsigned connected = xinput_hle_connected_count();
    if (connected == 0) {
        log_printer("xinput: all %u ports report EMPTY (honest default; %llu "
                    "enumeration quer%s answered no)\n",
                    (unsigned)XINPUT_PORT_COUNT, (unsigned long long)empty_queries,
                    empty_queries == 1u ? "y" : "ies");
    } else {
        log_printer("xinput: %u of %u ports report a SYNTHETIC pad (FABRICATED -- "
                    "presence only, no button or axis data exists)\n",
                    connected, (unsigned)XINPUT_PORT_COUNT);
    }

    if (!xinput_hle_layout_usable()) {
        log_printer("xinput: guest state layout NOT DERIVED -- controller state is kept "
                    "HOST-SIDE only and is never marshalled into guest memory (%llu "
                    "write%s refused). XINPUT_STATE and XINPUT_GAMEPAD are not in "
                    "docs/guest-structs.md and are NOT guessed here.\n",
                    (unsigned long long)write_refusals,
                    write_refusals == 1u ? "" : "s");
        /* The measured layout exists and was not taken up. Say so, because "not
         * derived" and "derived but not adopted" are different situations and only one
         * of them is waiting on analysis work. */
        log_printer("xinput: a MEASURED layout IS available (%u bytes, %d fields) and "
                    "has NOT been adopted -- adopt it if you want guest writes\n",
                    (unsigned)XINPUT_MEASURED_STATE_SIZE, (int)XINPUT_FIELD_COUNT);
    } else {
        log_printer("xinput: guest state layout %s -- %u byte structure, %u of %d "
                    "fields mapped\n",
                    measured_layout_adopted ? "ADOPTED from the measurement"
                                            : "DERIVED by hand",
                    state_size, xinput_hle_mapped_field_count(),
                    (int)XINPUT_FIELD_COUNT);
    }

    if (synthetic_values > 0) {
        log_printer("xinput: %llu FABRICATED button/axis value set%s installed -- this "
                    "run's input did not come from a controller\n",
                    (unsigned long long)synthetic_values,
                    synthetic_values == 1u ? "" : "s");
    }

    if (unknown_calls > 0) {
        log_printer("xinput: %llu call%s landed on an address absent from the surface "
                    "table -- the table and the binary DISAGREE\n",
                    (unsigned long long)unknown_calls,
                    unknown_calls == 1u ? "" : "s");
    }

    /* Selection sort. Fifteen entries, once per run, so clarity beats cleverness --
     * the same trade `dsound_hle_report` and `kernel_hle_report_missing` make. */
    bool emitted[XINPUT_FUNCTION_COUNT] = {false};
    for (size_t printed = 0; printed < missing; printed++) {
        size_t best = 0;
        bool found = false;
        for (size_t i = 0; i < XINPUT_FUNCTION_COUNT; i++) {
            if (emitted[i] || entries[i].state == XINPUT_ENTRY_IMPLEMENTED) {
                continue;
            }
            if (!found || ranks_above(&entries[i], &entries[best])) {
                found = true;
                best = i;
            }
        }
        if (!found) {
            break;
        }
        emitted[best] = true;
        char label[64];
        log_printer("  %#010x  %-32s %-12s sites %2u  calls %llu\n",
                    entries[best].address,
                    xinput_hle_entry_label(&entries[best], label, sizeof(label)),
                    xinput_hle_kind_name(entries[best].kind), entries[best].sites,
                    (unsigned long long)entries[best].call_count);
    }
}

/* ===================== CROSS-CHECK ===================== */

unsigned xinput_hle_crosscheck(const xinput_surface_ref *refs, size_t count)
{
    ensure_initialised();
    if (!refs && count > 0) {
        log_printer("xinput: crosscheck given %zu rows and a NULL pointer\n", count);
        return 1u;
    }

    unsigned disagreements = 0;

    for (size_t i = 0; i < count; i++) {
        const xinput_entry *mine = xinput_hle_entry(refs[i].address);
        if (!mine) {
            disagreements++;
            log_printer("xinput: crosscheck -- generated table has %#010x (%s, %u "
                        "sites) and we do not\n",
                        refs[i].address, refs[i].name ? refs[i].name : "unnamed",
                        refs[i].sites);
            continue;
        }
        if (mine->sites != refs[i].sites) {
            disagreements++;
            log_printer("xinput: crosscheck -- %#010x site count %u here, %u in the "
                        "generated table\n",
                        refs[i].address, mine->sites, refs[i].sites);
        }
        bool names_agree = (mine->name == NULL && refs[i].name == NULL) ||
                           (mine->name && refs[i].name &&
                            strcmp(mine->name, refs[i].name) == 0);
        if (!names_agree) {
            disagreements++;
            log_printer("xinput: crosscheck -- %#010x named \"%s\" here, \"%s\" in the "
                        "generated table\n",
                        refs[i].address, mine->name ? mine->name : "(null)",
                        refs[i].name ? refs[i].name : "(null)");
        }
    }

    /* The other direction. A check that only walked `refs` would pass against a table
     * of ours that had grown an invented row, which is the drift most likely to happen
     * by hand. Against an unmodified generated table this direction reports exactly
     * two: the zero-site rows the generator does not emit. */
    for (size_t i = 0; i < XINPUT_FUNCTION_COUNT; i++) {
        bool present = false;
        for (size_t j = 0; j < count; j++) {
            if (refs[j].address == entries[i].address) {
                present = true;
                break;
            }
        }
        if (!present) {
            disagreements++;
            char label[64];
            log_printer("xinput: crosscheck -- we have %#010x (%s) and the generated "
                        "table does not\n",
                        entries[i].address,
                        xinput_hle_entry_label(&entries[i], label, sizeof(label)));
        }
    }

    return disagreements;
}

/* Port state snapshots and updates are serialized independently of guest tables. */
void xinput_hle_init(void)
{ state_lock(); locked_xinput_hle_init(); state_unlock(); }
bool xinput_hle_attach_synthetic_pad(unsigned port)
{ state_lock(); bool result = locked_xinput_hle_attach_synthetic_pad(port); state_unlock(); return result; }
bool xinput_hle_detach_synthetic_pad(unsigned port)
{ state_lock(); bool result = locked_xinput_hle_detach_synthetic_pad(port); state_unlock(); return result; }
xinput_port_state xinput_hle_port_state(unsigned port)
{ state_lock(); xinput_port_state result = locked_xinput_hle_port_state(port); state_unlock(); return result; }
bool xinput_hle_port_connected(unsigned port)
{ state_lock(); bool result = locked_xinput_hle_port_connected(port); state_unlock(); return result; }
unsigned xinput_hle_connected_count(void)
{ state_lock(); unsigned result = locked_xinput_hle_connected_count(); state_unlock(); return result; }
xinput_pad_state xinput_hle_pad_state(unsigned port)
{ state_lock(); xinput_pad_state result = locked_xinput_hle_pad_state(port); state_unlock(); return result; }
bool xinput_hle_set_synthetic_pad_state(unsigned port, xinput_pad_state state)
{ state_lock(); bool result = locked_xinput_hle_set_synthetic_pad_state(port, state); state_unlock(); return result; }
uint32_t xinput_hle_synthetic_report_changes(unsigned port)
{ state_lock(); uint32_t result = locked_xinput_hle_synthetic_report_changes(port); state_unlock(); return result; }
unsigned xinput_hle_write_guest_state(unsigned port, uint32_t guest_address)
{ state_lock(); unsigned result = locked_xinput_hle_write_guest_state(port, guest_address); state_unlock(); return result; }
