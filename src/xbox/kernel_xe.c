/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * XBE section residency: ordinals 327 XeLoadSection and 328 XeUnloadSection.
 *
 * All of the evidence -- the arity derivation, the measured struct layout, and the
 * list of what is deliberately not modelled -- is in kernel_xe.h. This file is the
 * bookkeeping.
 */

#include "kernel_xe.h"

#include "kernel_hle.h"
#include "nt_status.h"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Resolved from tools/kernel_ordinals.py lines 385-386, never from memory. */
#define ORD_XE_LOAD_SECTION 327u
#define ORD_XE_UNLOAD_SECTION 328u

/* XBE_HEADER offsets, MEASURED against build/default.xbe -- see kernel_xe.h. */
#define XBE_HEADER_NUMBER_OF_SECTIONS 0x11Cu
#define XBE_HEADER_SECTION_HEADERS_ADDRESS 0x120u
/* 'XBEH' little-endian. Same constant as XBE_MAGIC in src/loader/xbe.h, repeated
 * rather than included: tsfp_xbox does not link tsfp_loader, and adding that
 * dependency to gain one literal would couple the HLE to the image parser. */
#define XBE_MAGIC_XBEH 0x48454258u

/* XBE_SECTION_HEADER offsets. +0x04, +0x08 and +0x14 are read by guest instructions;
 * +0x18 is bracketed by the shared-page counter chain. kernel_xe.h has both proofs. */
#define SECTION_FLAGS 0x00u
#define SECTION_VIRTUAL_ADDRESS 0x04u
#define SECTION_VIRTUAL_SIZE 0x08u
#define SECTION_NAME_ADDRESS 0x14u
#define SECTION_REFERENCE_COUNT 0x18u

#define XBE_SECTION_FLAG_PRELOAD 0x02u

/*
 * A refusal bound on NumberOfSections, not a silent clamp.
 *
 * src/loader/xbe.h caps its own table at 64 and calls anything larger an error. The
 * format allows more, so this is deliberately looser -- but it must exist, because
 * `table + count * 0x38` with an unchecked count read out of guest memory is an
 * overflow, and an overflowed bound would make a wild handle test as IN range.
 */
#define SECTION_COUNT_MAX 4096u

/* Longest section name we will quote in a log line. The image's longest is
 * "$$XTIMAGE"; XBE_SECTION_NAME_MAX in the loader is 32. */
#define SECTION_NAME_MAX 32u

/*
 * ONE RECURSIVE LOCK, following kernel_hal.c, kernel_object.c and guest_mem.c.
 *
 * Needed, not defensive: the reference count is a read-modify-write of GUEST memory,
 * and the title reaches these ordinals on thread 2 while thread 1 is still live. Two
 * concurrent loads of the same section without the lock can both read N and both write
 * N+1, and the count is then one short forever -- which surfaces much later as an
 * underflow on the matching unload, with this module's own diagnostic blaming the
 * guest for our bug. That is the failure mode kernel_object.c documents.
 *
 * RECURSIVE because the critical section calls kernel_hle_log(), whose sink is
 * caller-supplied and may re-enter.
 */
static pthread_mutex_t xe_lock;
static bool xe_lock_ready;
static pthread_once_t xe_lock_once = PTHREAD_ONCE_INIT;

static void xe_lock_init(void)
{
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        return;
    }
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0 &&
        pthread_mutex_init(&xe_lock, &attr) == 0) {
        xe_lock_ready = true;
    }
    (void)pthread_mutexattr_destroy(&attr);
}

static void lock(void)
{
    (void)pthread_once(&xe_lock_once, xe_lock_init);
    if (xe_lock_ready) {
        (void)pthread_mutex_lock(&xe_lock);
    }
}

static void unlock(void)
{
    if (xe_lock_ready) {
        (void)pthread_mutex_unlock(&xe_lock);
    }
}

/*
 * HOST STATE IS COUNTERS ONLY.
 *
 * The reference count itself lives in guest memory at Section+0x18 and is never
 * mirrored here. Two sources of truth for one number is how a bookkeeping bug survives
 * its own tests: a shadow count would stay self-consistent while the guest's copy drifted.
 */
static kernel_guest_ptr image_base;
static unsigned load_count;
static unsigned unload_count;
static unsigned underflow_count;
static unsigned refused_count;
static unsigned not_resident_count;
static unsigned stale_resident_count;
/* So the announcements below are made once rather than per call. They are warnings
 * about the HOST's configuration, and a warning repeated 400 times is noise that
 * hides the next one. */
static bool warned_no_image_base;
static bool warned_already_mapped;

static size_t host_page_size(void)
{
    const long page = sysconf(_SC_PAGESIZE);
    return (page > 0L) ? (size_t)page : 4096u;
}

/*
 * Is the section's own virtual range actually readable in this host process?
 *
 * kernel_guest_at() probes each requested range; this probe checks the entire
 * section body before XeLoadSection reports success.
 *
 * The check is kernel_guest_range_readable() (kernel_call.c), a process_vm_readv
 * probe shared with every guest accessor. It replaced an msync(MS_ASYNC) probe here,
 * which verified mapping but not read permission: a PROT_NONE page passed it and the
 * title's first read still faulted (T81). The readv probe refuses unmapped AND
 * unreadable pages without touching guest bytes or raising a signal. The residual
 * limit is shared too and is documented on the probe itself: it is a check, not a
 * lock, so a concurrent unmap can invalidate the result before a subsequent access,
 * and that fault lands in host_runtime.c's structured fault handler.
 */
static bool pages_resident(kernel_guest_ptr addr, uint32_t bytes, int *probe_errno)
{
    *probe_errno = 0;
    if (addr == 0u) {
        return false;
    }
    const size_t page = host_page_size();
    const uintptr_t mask = ~(uintptr_t)(page - 1u);
    const uintptr_t start = (uintptr_t)addr & mask;
    uintptr_t end = ((uintptr_t)addr + (uintptr_t)bytes + (uintptr_t)page - 1u) & mask;
    if (end <= start) {
        /* A zero-size section still occupies the page its VirtualAddress names, so
         * probe that one page rather than calling a zero-length range resident. */
        end = start + (uintptr_t)page;
    }
    errno = 0;
    if (kernel_guest_range_readable((kernel_guest_ptr)start, (size_t)(end - start))) {
        return true;
    }
    /* A short process_vm_readv result reports success for its prefix and sets no
     * errno, so a refusal without one is still named EFAULT rather than 0. */
    *probe_errno = (errno != 0) ? errno : EFAULT;
    return false;
}

/* Copy a NUL-terminated section name out of guest memory, for log lines only. Always
 * leaves `out` a valid C string, so a bad name address degrades the message rather
 * than the call. */
static void read_section_name(kernel_guest_ptr section, char *out, size_t out_bytes)
{
    out[0] = '\0';
    uint32_t name_addr = 0u;
    if (!kernel_guest_read_u32(section + SECTION_NAME_ADDRESS, &name_addr) ||
        name_addr == 0u) {
        (void)snprintf(out, out_bytes, "<no name>");
        return;
    }
    for (size_t i = 0u; i + 1u < out_bytes; i++) {
        uint8_t byte = 0u;
        if (!kernel_guest_read_u8(name_addr + (uint32_t)i, &byte) || byte == 0u) {
            out[i] = '\0';
            return;
        }
        out[i] = (char)byte;
        out[i + 1u] = '\0';
    }
}

void kernel_xe_set_image_base(kernel_guest_ptr xbe_base)
{
    lock();
    image_base = xbe_base;
    warned_no_image_base = false;
    unlock();
}

kernel_guest_ptr kernel_xe_image_base(void)
{
    lock();
    const kernel_guest_ptr base = image_base;
    unlock();
    return base;
}

bool kernel_xe_section_table(kernel_guest_ptr *table, uint32_t *count)
{
    if (table == NULL || count == NULL) {
        return false;
    }
    lock();
    const kernel_guest_ptr base = image_base;
    unlock();
    if (base == 0u) {
        return false;
    }

    uint32_t magic = 0u;
    if (!kernel_guest_read_u32(base, &magic) || magic != XBE_MAGIC_XBEH) {
        /* A base that is not an XBE is HOST WIRING broken, and it must not read as an
         * empty section table: an empty table would put every handle out of range and
         * fail every load for a reason that has nothing to do with the guest. */
        return false;
    }

    uint32_t sections = 0u;
    uint32_t headers = 0u;
    if (!kernel_guest_read_u32(base + XBE_HEADER_NUMBER_OF_SECTIONS, &sections) ||
        !kernel_guest_read_u32(base + XBE_HEADER_SECTION_HEADERS_ADDRESS, &headers)) {
        return false;
    }
    if (sections == 0u || sections > SECTION_COUNT_MAX || headers == 0u) {
        return false;
    }
    /* Refused, not truncated. `headers + sections * 0x38` must not wrap, or the bound
     * below would admit a handle far outside the image. */
    if (sections > (0xFFFFFFFFu - headers) / KERNEL_XE_SECTION_HEADER_BYTES) {
        return false;
    }
    *table = headers;
    *count = sections;
    return true;
}

bool kernel_xe_section_index(kernel_guest_ptr section, uint32_t *index)
{
    if (index == NULL || section == 0u) {
        return false;
    }
    kernel_guest_ptr table = 0u;
    uint32_t count = 0u;
    if (!kernel_xe_section_table(&table, &count)) {
        return false;
    }
    if (section < table) {
        return false;
    }
    const uint32_t offset = section - table;
    /* The stride check is not pedantry. A pointer into the middle of a header reads
     * every field at the wrong offset and every value still looks plausible: a
     * VirtualSize would be read as a VirtualAddress and the load would "succeed". */
    if (offset % KERNEL_XE_SECTION_HEADER_BYTES != 0u) {
        return false;
    }
    const uint32_t slot = offset / KERNEL_XE_SECTION_HEADER_BYTES;
    if (slot >= count) {
        return false;
    }
    *index = slot;
    return true;
}

bool kernel_xe_section_reference_count(kernel_guest_ptr section, uint32_t *count)
{
    if (count == NULL || section == 0u) {
        return false;
    }
    return kernel_guest_read_u32(section + SECTION_REFERENCE_COUNT, count);
}

void kernel_xe_reset(void)
{
    lock();
    load_count = 0u;
    unload_count = 0u;
    underflow_count = 0u;
    refused_count = 0u;
    not_resident_count = 0u;
    stale_resident_count = 0u;
    warned_no_image_base = false;
    warned_already_mapped = false;
    /* The IMAGE BASE is deliberately NOT cleared. It is host wiring installed once
     * before guest code runs, exactly like kernel_hal.c's firmware sink, and a reset
     * that silently detached it would disable the bounds check for every later call
     * while reporting nothing. */
    unlock();
}

unsigned kernel_xe_load_count(void)
{
    lock();
    const unsigned value = load_count;
    unlock();
    return value;
}

unsigned kernel_xe_unload_count(void)
{
    lock();
    const unsigned value = unload_count;
    unlock();
    return value;
}

unsigned kernel_xe_underflow_count(void)
{
    lock();
    const unsigned value = underflow_count;
    unlock();
    return value;
}

unsigned kernel_xe_refused_count(void)
{
    lock();
    const unsigned value = refused_count;
    unlock();
    return value;
}

unsigned kernel_xe_not_resident_count(void)
{
    lock();
    const unsigned value = not_resident_count;
    unlock();
    return value;
}

unsigned kernel_xe_stale_resident_count(void)
{
    lock();
    const unsigned value = stale_resident_count;
    unlock();
    return value;
}

/*
 * Shared argument handling for both ordinals.
 *
 * Returns STATUS_SUCCESS when `section` is usable, and on failure has already counted
 * and reported. `index_out` receives the section index, or SECTION_INDEX_UNKNOWN when
 * no image base was supplied -- which is announced rather than treated as index 0.
 */
#define SECTION_INDEX_UNKNOWN 0xFFFFFFFFu

static uint32_t take_section_argument(void *context, const char *who,
                                      kernel_guest_ptr *section_out,
                                      uint32_t *index_out)
{
    *section_out = 0u;
    *index_out = SECTION_INDEX_UNKNOWN;

    if (context == NULL) {
        lock();
        refused_count++;
        unlock();
        kernel_hle_log()("kernel: %s called with no argument frame -- the call boundary "
                         "did not supply one\n", who);
        return STATUS_INVALID_PARAMETER;
    }

    uint32_t section = 0u;
    if (!kernel_frame_arg((const kernel_call_frame *)context, 0u, &section)) {
        lock();
        refused_count++;
        unlock();
        kernel_hle_log()("kernel: %s could not read its section argument from the guest "
                         "stack\n", who);
        return STATUS_INVALID_PARAMETER;
    }

    /* 0xFFFFFFFF is what _XGetSectionHandleA@4 returns for a name it did not find
     * (`or eax, 0xFFFFFFFF` at 0x0037C96D). The title checks for it at 0x00381096
     * before calling, so seeing it here means either a path that forgot to check or
     * our own argument fetch is wrong. Named explicitly so the two are not confused. */
    if (section == 0xFFFFFFFFu) {
        lock();
        refused_count++;
        unlock();
        kernel_hle_log()("kernel: %s(0xFFFFFFFF) -- that is _XGetSectionHandleA's "
                         "NOT-FOUND value, not a section handle\n", who);
        return STATUS_INVALID_HANDLE;
    }
    if (section == 0u) {
        lock();
        refused_count++;
        unlock();
        kernel_hle_log()("kernel: %s(NULL) -- a null section header is not a thing the "
                         "guest does; every measured site passes a handle from "
                         "_XGetSectionHandleA@4\n", who);
        return STATUS_INVALID_HANDLE;
    }
    /* The whole header must be readable before any field is. Reading +0x18 out of a
     * header whose tail falls off the mapping would be the wrong kind of lucky. */
    if (kernel_guest_at(section, KERNEL_XE_SECTION_HEADER_BYTES) == NULL) {
        lock();
        refused_count++;
        unlock();
        kernel_hle_log()("kernel: %s(0x%08X) -- that is not a readable guest address for "
                         "a %u-byte XBE_SECTION_HEADER\n", who, (unsigned)section,
                         (unsigned)KERNEL_XE_SECTION_HEADER_BYTES);
        return STATUS_INVALID_HANDLE;
    }

    kernel_guest_ptr table = 0u;
    uint32_t count = 0u;
    if (!kernel_xe_section_table(&table, &count)) {
        /* No usable image header. The reference count still works -- it lives in the
         * handle's own structure -- but the handle cannot be checked against anything,
         * and that gap is ANNOUNCED rather than passed off as a check. */
        lock();
        const bool first = !warned_no_image_base;
        warned_no_image_base = true;
        unlock();
        if (first) {
            kernel_hle_log()("kernel: %s has no usable XBE image header, so a section "
                             "handle CANNOT BE BOUNDS-CHECKED -- the check is DISABLED "
                             "for this run. The host must call "
                             "kernel_xe_set_image_base(image.base_address) before guest "
                             "code starts. Reference counting is unaffected\n", who);
        }
        *section_out = section;
        return STATUS_SUCCESS;
    }

    uint32_t index = 0u;
    if (!kernel_xe_section_index(section, &index)) {
        lock();
        refused_count++;
        unlock();
        kernel_hle_log()("kernel: %s(0x%08X) REFUSED -- not a section of this image. The "
                         "section table is %u entr%s of %u bytes at 0x%08X, so a handle "
                         "must be in [0x%08X, 0x%08X) and a multiple of %u from the "
                         "start\n", who, (unsigned)section, (unsigned)count,
                         count == 1u ? "y" : "ies",
                         (unsigned)KERNEL_XE_SECTION_HEADER_BYTES, (unsigned)table,
                         (unsigned)table,
                         (unsigned)(table + count * KERNEL_XE_SECTION_HEADER_BYTES),
                         (unsigned)KERNEL_XE_SECTION_HEADER_BYTES);
        return STATUS_INVALID_HANDLE;
    }

    *section_out = section;
    *index_out = index;
    return STATUS_SUCCESS;
}

/* "section 13" when the index is known, "section ?" when the bounds check is off. So a
 * log line never implies a validation that did not happen. */
static void format_index(uint32_t index, char *out, size_t out_bytes)
{
    if (index == SECTION_INDEX_UNKNOWN) {
        (void)snprintf(out, out_bytes, "section ?");
    } else {
        (void)snprintf(out, out_bytes, "section %u", (unsigned)index);
    }
}

/*
 * ARITY-OK(327): ONE stack argument, __stdcall. Derived three ways in kernel_xe.h --
 * a forced stack balance through `pop esi; ret 4` at the one site, the `mov esi,[esp+8]`
 * that proves the first push is a callee-save, and nxdk's `XeLoadSection@4`. The
 * measured row {327u, 2u, 1u, 1} says TWO and is WRONG: its single-site bracket opens
 * on the function prologue and counts `push esi` as an argument.
 *
 * Returns NTSTATUS. The caller tests the sign (`test eax,eax; jge`), so any negative
 * value takes its failure path and it returns NULL to ITS caller.
 */
static uint32_t hle_xe_load_section(void *context)
{
    kernel_guest_ptr section = 0u;
    uint32_t index = SECTION_INDEX_UNKNOWN;
    const uint32_t refusal = take_section_argument(context, "XeLoadSection", &section,
                                                   &index);
    if (refusal != STATUS_SUCCESS) {
        return refusal;
    }

    char where[32];
    char name[SECTION_NAME_MAX];
    format_index(index, where, sizeof(where));
    read_section_name(section, name, sizeof(name));

    uint32_t flags = 0u;
    uint32_t virtual_addr = 0u;
    uint32_t virtual_size = 0u;
    (void)kernel_guest_read_u32(section + SECTION_FLAGS, &flags);
    (void)kernel_guest_read_u32(section + SECTION_VIRTUAL_ADDRESS, &virtual_addr);
    (void)kernel_guest_read_u32(section + SECTION_VIRTUAL_SIZE, &virtual_size);

    /* CHECKED, NOT ASSUMED. This is the one promise XeLoadSection makes, and the one
     * thing a bookkeeping-only implementation would get wrong without noticing: if the
     * host never mapped these pages, returning success hands the guest a VirtualAddress
     * that faults on first read, with the fault landing nowhere near this call. */
    int probe_errno = 0;
    if (!pages_resident(virtual_addr, virtual_size, &probe_errno)) {
        lock();
        not_resident_count++;
        unlock();
        kernel_hle_log()("kernel: XeLoadSection(0x%08X, %s \"%s\") REFUSED -- its %u "
                         "bytes at 0x%08X are NOT MAPPED in this host (probe errno %d). "
                         "Nothing here can page a section in, so returning success "
                         "would hand the title an address that faults on first read\n",
                         (unsigned)section, where, name, (unsigned)virtual_size,
                         (unsigned)virtual_addr, probe_errno);
        return STATUS_INVALID_PARAMETER;
    }

    lock();
    uint32_t references = 0u;
    if (!kernel_guest_read_u32(section + SECTION_REFERENCE_COUNT, &references)) {
        refused_count++;
        unlock();
        kernel_hle_log()("kernel: XeLoadSection(0x%08X, %s) could not read the reference "
                         "count at +0x18\n", (unsigned)section, where);
        return STATUS_INVALID_HANDLE;
    }
    if (references == 0xFFFFFFFFu) {
        /* Refused rather than wrapped. A wrap to 0 would make the very next unload
         * underflow and blame the guest. */
        refused_count++;
        unlock();
        kernel_hle_log()("kernel: XeLoadSection(0x%08X, %s \"%s\") REFUSED -- the "
                         "reference count is already 0xFFFFFFFF and incrementing it "
                         "would wrap to zero\n", (unsigned)section, where, name);
        return STATUS_INVALID_PARAMETER;
    }
    if (!kernel_guest_write_u32(section + SECTION_REFERENCE_COUNT, references + 1u)) {
        refused_count++;
        unlock();
        kernel_hle_log()("kernel: XeLoadSection(0x%08X, %s) could not WRITE the reference "
                         "count at +0x18 -- the count is now wrong and the matching "
                         "unload will underflow\n", (unsigned)section, where);
        return STATUS_INVALID_HANDLE;
    }
    load_count++;
    const bool first_load = (references == 0u);
    const bool announce_mapping = first_load && !warned_already_mapped;
    if (announce_mapping) {
        warned_already_mapped = true;
    }
    unlock();

    kernel_hle_log()("kernel: XeLoadSection(0x%08X, %s \"%s\") -> reference count %u, "
                     "%u bytes at 0x%08X, flags 0x%02X (%s)\n", (unsigned)section, where,
                     name, (unsigned)(references + 1u), (unsigned)virtual_size,
                     (unsigned)virtual_addr, (unsigned)(flags & 0xFFu),
                     (flags & XBE_SECTION_FLAG_PRELOAD) != 0u ? "PRELOAD"
                                                              : "demand-paged");
    if (announce_mapping) {
        /* Said once, because a wrong implementation here is INVISIBLE: the pages are
         * already there, so success looks right no matter what the count does. */
        kernel_hle_log()("kernel: that was a 0 -> 1 transition, which on hardware is "
                         "where the pages get committed and the body is read from the "
                         "XBE. NOTHING WAS PAGED IN: xbe_map() already copied every "
                         "section, preload or not, so this call is reference counting "
                         "and a residency check, nothing more\n");
    }
    return STATUS_SUCCESS;
}

/*
 * ARITY-OK(328): ONE stack argument, __stdcall. The measured row {328u, 1u, 1u, 1} has
 * the right number but one site, so stack_args_for() refuses it and a hand row is still
 * required. Derived by forced stack balance through `ret 4` at 0x0037C9B7, and
 * corroborated by nxdk's `XeUnloadSection@4`. See kernel_xe.h.
 *
 * Returns NTSTATUS. The caller tests the sign and returns TRUE/FALSE accordingly.
 */
static uint32_t hle_xe_unload_section(void *context)
{
    kernel_guest_ptr section = 0u;
    uint32_t index = SECTION_INDEX_UNKNOWN;
    const uint32_t refusal = take_section_argument(context, "XeUnloadSection", &section,
                                                   &index);
    if (refusal != STATUS_SUCCESS) {
        return refusal;
    }

    char where[32];
    char name[SECTION_NAME_MAX];
    format_index(index, where, sizeof(where));
    read_section_name(section, name, sizeof(name));

    lock();
    uint32_t references = 0u;
    if (!kernel_guest_read_u32(section + SECTION_REFERENCE_COUNT, &references)) {
        refused_count++;
        unlock();
        kernel_hle_log()("kernel: XeUnloadSection(0x%08X, %s) could not read the "
                         "reference count at +0x18\n", (unsigned)section, where);
        return STATUS_INVALID_HANDLE;
    }
    if (references == 0u) {
        /*
         * REPORTED, NOT CLAMPED QUIETLY. This is the single most valuable diagnostic in
         * the module. An XeLoadSection that returned success without incrementing would
         * look perfect -- the pages are already mapped, so the title gets a working
         * address either way -- right up to here. So an underflow is counted, named, and
         * answered with a FAILURE status rather than a success the count cannot justify.
         * The count stays at 0 instead of wrapping to 0xFFFFFFFF, because a wrapped
         * count would then let 4 billion unmatched unloads succeed.
         */
        underflow_count++;
        unlock();
        kernel_hle_log()("kernel: XeUnloadSection(0x%08X, %s \"%s\") UNDERFLOW -- the "
                         "reference count is already 0, so this unload has no matching "
                         "load. Left at 0 and REFUSED rather than wrapped. Either the "
                         "title is unbalanced or ordinal 327 is not counting\n",
                         (unsigned)section, where, name);
        return STATUS_INVALID_PARAMETER;
    }
    if (!kernel_guest_write_u32(section + SECTION_REFERENCE_COUNT, references - 1u)) {
        refused_count++;
        unlock();
        kernel_hle_log()("kernel: XeUnloadSection(0x%08X, %s) could not WRITE the "
                         "reference count at +0x18\n", (unsigned)section, where);
        return STATUS_INVALID_HANDLE;
    }
    unload_count++;
    const bool last_unload = (references == 1u);
    if (last_unload) {
        stale_resident_count++;
    }
    unlock();

    kernel_hle_log()("kernel: XeUnloadSection(0x%08X, %s \"%s\") -> reference count %u\n",
                     (unsigned)section, where, name, (unsigned)(references - 1u));
    if (last_unload) {
        /* Every time, not once. Each one is a section the title believes is gone and
         * which is still readable, so a later read that should have faulted will not. */
        kernel_hle_log()("kernel: that was a 1 -> 0 transition. On hardware the pages "
                         "would now be DECOMMITTED; they are still mapped here, so a "
                         "read of this section after its last unload will quietly "
                         "succeed where hardware would fault. %u such section(s) so "
                         "far\n", kernel_xe_stale_resident_count());
    }
    return STATUS_SUCCESS;
}

unsigned kernel_xe_register(void)
{
    static const struct {
        unsigned ordinal;
        kernel_fn handler;
    } bindings[] = {
        {ORD_XE_LOAD_SECTION, hle_xe_load_section},
        {ORD_XE_UNLOAD_SECTION, hle_xe_unload_section},
    };

    unsigned bound = 0u;
    for (size_t i = 0u; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        if (kernel_hle_register(bindings[i].ordinal, bindings[i].handler)) {
            bound++;
        }
    }
    return bound;
}
