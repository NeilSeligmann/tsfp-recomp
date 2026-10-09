/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xnet_hle.h"
#include "kernel_crypto.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#if __has_include("xdk_surface.h")
#include "xdk_surface.h"
_Static_assert(sizeof(xnet_xdk_row) == sizeof(xdk_surface_entry),
               "xnet_xdk_row size has drifted from the generated xdk_surface_entry");
_Static_assert(offsetof(xnet_xdk_row, address) == offsetof(xdk_surface_entry, address),
               "xnet_xdk_row.address offset has drifted from xdk_surface_entry");
_Static_assert(offsetof(xnet_xdk_row, section) == offsetof(xdk_surface_entry, section),
               "xnet_xdk_row.section offset has drifted from xdk_surface_entry");
_Static_assert(offsetof(xnet_xdk_row, name) == offsetof(xdk_surface_entry, name),
               "xnet_xdk_row.name offset has drifted from xdk_surface_entry");
_Static_assert(offsetof(xnet_xdk_row, sites) == offsetof(xdk_surface_entry, sites),
               "xnet_xdk_row.sites offset has drifted from xdk_surface_entry");
#endif
#endif

static xnet_entry *entries;
static size_t entry_count;
static uint64_t unknown_count;
static xnet_fatal_fn fatal_handler;
static bool lifecycle_bound;
static int default_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int result = vfprintf(stderr, format, args);
    va_end(args);
    return result;
}
static xnet_log_fn printer = default_printer;
void xnet_hle_set_log(xnet_log_fn value) { printer = value ? value : default_printer; }
xnet_log_fn xnet_hle_log(void) { return printer; }
void xnet_hle_set_fatal(xnet_fatal_fn value) { fatal_handler = value; }
void xnet_hle_fatal(uint32_t address, const char *format, ...)
{
    char message[256];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    printer("xnet: FATAL at guest address 0x%08x: %s\n", (unsigned)address, message);
    if (fatal_handler) fatal_handler(address, message);
    abort();
}
static int compare_address(const void *left, const void *right)
{
    const uint32_t a = ((const xnet_entry *)left)->address;
    const uint32_t b = ((const xnet_entry *)right)->address;
    return a < b ? -1 : a > b ? 1 : 0;
}
bool xnet_hle_init(const xnet_surface_entry *rows, size_t count)
{
    if (!rows || count == 0u || count > SIZE_MAX / sizeof(xnet_entry)) return false;
    xnet_entry *adopted = calloc(count, sizeof(*adopted));
    if (!adopted) return false;
    for (size_t i = 0u; i < count; i++) {
        if (rows[i].address == 0u) { free(adopted); return false; }
        adopted[i].address = rows[i].address;
        adopted[i].name = rows[i].name;
        adopted[i].sites = rows[i].sites;
    }
    qsort(adopted, count, sizeof(*adopted), compare_address);
    for (size_t i = 1u; i < count; i++) {
        if (adopted[i - 1u].address == adopted[i].address) { free(adopted); return false; }
    }
    free(entries);
    entries = adopted;
    entry_count = count;
    unknown_count = 0u;
    lifecycle_bound = false;
    return true;
}
bool xnet_hle_adopt(const xnet_xdk_row *rows, size_t count)
{
    if (!rows || count == 0u || count > SIZE_MAX / sizeof(xnet_surface_entry)) return false;
    xnet_surface_entry *selected = malloc(count * sizeof(*selected));
    if (!selected) return false;
    size_t matched = 0u;
    for (size_t i = 0u; i < count; i++) {
        if (rows[i].section && strcmp(rows[i].section, "XNET") == 0) {
            selected[matched++] = (xnet_surface_entry){rows[i].address, rows[i].name, rows[i].sites};
        }
    }
    const bool okay = xnet_hle_init(selected, matched);
    free(selected);
    return okay;
}
void xnet_hle_shutdown(void)
{
    lifecycle_bound = false;
    (void)xnet_hle_set_lifecycle_provider(NULL);
    free(entries);
    entries = NULL;
    entry_count = 0u;
    unknown_count = 0u;
}
static xnet_entry *find_entry(uint32_t address)
{
    size_t low = 0u, high = entry_count;
    while (low < high) {
        const size_t mid = low + (high - low) / 2u;
        if (entries[mid].address == address) return &entries[mid];
        if (entries[mid].address < address) low = mid + 1u;
        else high = mid;
    }
    return NULL;
}
const xnet_entry *xnet_hle_entry(uint32_t address) { return find_entry(address); }
bool xnet_hle_register(uint32_t address, xnet_fn handler)
{
    xnet_entry *entry = find_entry(address);
    if (!entry || !handler) return false;
    entry->handler = handler;
    entry->state = XNET_ENTRY_IMPLEMENTED;
    return true;
}
bool xnet_hle_set_default_return(uint32_t address, uint32_t value)
{
    xnet_entry *entry = find_entry(address);
    if (!entry) return false;
    entry->default_return = value;
    return true;
}
uint32_t xnet_hle_call(uint32_t address, void *context)
{
    xnet_entry *entry = find_entry(address);
    if (!entry) {
        unknown_count++;
        printer("xnet: call to UNKNOWN address 0x%08x -- not in measured surface\n", (unsigned)address);
        return 0u;
    }
    entry->call_count++;
    if (entry->handler) return entry->handler(context);
    if (!entry->reported) {
        entry->reported = true;
        printer("xnet: missing %s at 0x%08x\n", entry->name ? entry->name : "unnamed", (unsigned)address);
    }
    return entry->default_return;
}
size_t xnet_hle_count(void) { return entry_count; }
size_t xnet_hle_implemented_count(void)
{
    size_t count = 0u;
    for (size_t i = 0u; i < entry_count; i++)
        if (entries[i].state == XNET_ENTRY_IMPLEMENTED) count++;
    return count;
}
uint64_t xnet_hle_unknown_count(void) { return unknown_count; }
void xnet_hle_report_backlog(void)
{
    for (size_t i = 0u; i < entry_count; i++) {
        if (entries[i].state == XNET_ENTRY_STUB)
            printer("xnet: pending 0x%08x %s: %u sites, %llu calls\n",
                    (unsigned)entries[i].address, entries[i].name ? entries[i].name : "unnamed",
                    (unsigned)entries[i].sites, (unsigned long long)entries[i].call_count);
    }
}

/* T1091: the outer lifecycle from 004319AB/00431BE3. The lower constructor is
 * an explicit integration boundary, not an unconditional-success NIC stub. */
#include "kernel_call.h"
#include "kernel_sync.h"
#include <stdatomic.h>
#include <threads.h>

#define XNET_SINGLETON 0x007715ecu
#define XNET_LOCK 0x007715e8u
#define XNET_OBJECT_BYTES 0xd50u
#define XNET_REF 0xd30u
#define WSA_REF 0xd32u
static xnet_lifecycle_provider lifecycle;

bool xnet_hle_set_lifecycle_provider(const xnet_lifecycle_provider *provider)
{
    if (lifecycle_bound) return false;
    if (provider == NULL) {
        lifecycle = (xnet_lifecycle_provider){0};
        return true;
    }
    if (!provider->allocate || !provider->release || !provider->initialize ||
        !provider->close_sockets || !provider->destroy || !provider->set_last_error)
        return false;
    lifecycle = *provider;
    return true;
}

bool xnet_hle_prepare_object(uint32_t object)
{
    const unsigned char zero[XNET_OBJECT_BYTES] = {0};
    if (!kernel_guest_write_bytes(object, zero, sizeof(zero)) ||
        !kernel_guest_write_u32(kernel_guest_add(object, 0x264u), 0x004a1e00u))
        return false;
    /* 0032830 calls 004318AD for every element: that constructor is MOV EAX,ECX;
     * RET and therefore leaves the already-zeroed storage unchanged. */
    return true;
}

static uint32_t lifecycle_arg(void *context, unsigned index, uint32_t entry)
{
    uint32_t value;
    if (!kernel_frame_arg(context, index, &value))
        xnet_hle_fatal(entry, "lifecycle stack argument %u is unreadable", index);
    return value;
}
static uint32_t lifecycle_read32(uint32_t address, uint32_t entry)
{
    uint32_t value;
    if (!kernel_guest_read_u32(address, &value))
        xnet_hle_fatal(entry, "lifecycle dword 0x%08x is unreadable", address);
    return value;
}
static uint16_t lifecycle_read16(uint32_t address, uint32_t entry)
{
    unsigned char bytes[2];
    if (!kernel_guest_read_bytes(address, bytes, sizeof(bytes)))
        xnet_hle_fatal(entry, "lifecycle word 0x%08x is unreadable", address);
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}
static void lifecycle_write16(uint32_t address, uint16_t value, uint32_t entry)
{
    const unsigned char bytes[2] = {(unsigned char)value, (unsigned char)(value >> 8)};
    if (!kernel_guest_write_bytes(address, bytes, sizeof(bytes)))
        xnet_hle_fatal(entry, "lifecycle word 0x%08x is unwritable", address);
}
static void lifecycle_write32(uint32_t address, uint32_t value, uint32_t entry)
{
    if (!kernel_guest_write_u32(address, value))
        xnet_hle_fatal(entry, "lifecycle dword 0x%08x is unwritable", address);
}
static void lifecycle_lock(uint32_t entry)
{
    _Atomic uint32_t *lock = kernel_guest_at(XNET_LOCK, sizeof(uint32_t));
    if (!lock) xnet_hle_fatal(entry, "singleton spinlock is unreadable");
    uint32_t expected = 0u;
    while (!atomic_compare_exchange_weak_explicit(lock, &expected, 1u,
                                                 memory_order_acquire, memory_order_relaxed)) {
        expected = 0u;
        thrd_yield(); /* Original Sleep(0), 003800D1. */
    }
}
static void lifecycle_unlock(uint32_t entry)
{
    _Atomic uint32_t *lock = kernel_guest_at(XNET_LOCK, sizeof(uint32_t));
    if (!lock) xnet_hle_fatal(entry, "singleton spinlock disappeared");
    atomic_store_explicit(lock, 0u, memory_order_release);
}
static uint32_t lifecycle_result(uint32_t error, bool wsa, uint32_t entry)
{
    if (error && wsa) {
        if (!lifecycle.set_last_error(error))
            xnet_hle_fatal(entry, "original TLS last-error slot is unwritable");
        return UINT32_MAX;
    }
    return error;
}
static uint32_t lifecycle_start(void *context, bool wsa)
{
    const uint32_t entry = wsa ? 0x00431bc8u : 0x00431bb1u;
    const uint32_t params = wsa ? 0u : lifecycle_arg(context, 0u, entry);
    const uint32_t version = wsa ? lifecycle_arg(context, 0u, entry) : 0u;
    const uint32_t output = wsa ? lifecycle_arg(context, 1u, entry) : 0u;
    lifecycle_lock(entry);
    uint32_t object = lifecycle_read32(XNET_SINGLETON, entry);
    uint32_t error = 0u;
    if (object == 0u) {
        object = lifecycle.allocate(XNET_OBJECT_BYTES, 0x4454454eu);
        if (object && !xnet_hle_prepare_object(object))
            xnet_hle_fatal(entry, "allocated stack object is unwritable");
        lifecycle_write32(XNET_SINGLETON, object, entry);
        if (!object) {
            error = 0x2747u; /* RtlNtStatusToDosError(80072747). */
        } else {
            error = lifecycle.initialize(object, params);
            if (error) {
                lifecycle.destroy(object, false);
                lifecycle_write32(XNET_SINGLETON, 0u, entry);
                /* MEASURED original failure path does NOT ExFreePool(object). */
            }
        }
    }
    if (!error) {
        const uint32_t ref = kernel_guest_add(object, wsa ? WSA_REF : XNET_REF);
        lifecycle_write16(ref, (uint16_t)(lifecycle_read16(ref, entry) + 1u), entry);
        if (wsa) {
            lifecycle_write16(output, (uint16_t)version, entry);
            lifecycle_write16(kernel_guest_add(output, 2u), 0x0202u, entry);
            lifecycle_write32(kernel_guest_add(output, 0x18cu), 0u, entry);
            lifecycle_write16(kernel_guest_add(output, 0x186u), 0u, entry);
            lifecycle_write16(kernel_guest_add(output, 0x188u), 0u, entry);
            if (!kernel_guest_write_u8(kernel_guest_add(output, 4u), 0u) ||
                !kernel_guest_write_u8(kernel_guest_add(output, 0x105u), 0u))
                xnet_hle_fatal(entry, "WSADATA string boundary is unwritable");
        }
    }
    lifecycle_unlock(entry);
    return lifecycle_result(error, wsa, entry);
}
static uint32_t lifecycle_finish(bool wsa)
{
    const uint32_t entry = wsa ? 0x00431cc3u : 0x00431cb6u;
    lifecycle_lock(entry);
    const uint32_t object = lifecycle_read32(XNET_SINGLETON, entry);
    uint32_t error = 0x276du;
    if (object) {
        const uint32_t ref = kernel_guest_add(object, wsa ? WSA_REF : XNET_REF);
        const uint16_t before = lifecycle_read16(ref, entry);
        if (before) {
            const uint16_t after = (uint16_t)(before - 1u);
            lifecycle_write16(ref, after, entry);
            if (wsa && !after) lifecycle.close_sockets(object);
            if (!lifecycle_read16(kernel_guest_add(object, XNET_REF), entry) &&
                !lifecycle_read16(kernel_guest_add(object, WSA_REF), entry)) {
                lifecycle.destroy(object, true);
                lifecycle.release(object);
                lifecycle_write32(XNET_SINGLETON, 0u, entry);
            }
            error = 0u;
        }
    }
    lifecycle_unlock(entry);
    return lifecycle_result(error, wsa, entry);
}
static uint32_t xnet_start_handler(void *context) { return lifecycle_start(context, false); }
static uint32_t wsa_start_handler(void *context) { return lifecycle_start(context, true); }
static uint32_t xnet_finish_handler(void *context) { (void)context; return lifecycle_finish(false); }
static uint32_t wsa_finish_handler(void *context) { (void)context; return lifecycle_finish(true); }
size_t xnet_hle_register_lifecycle(void)
{
    if (!lifecycle.allocate) return 0u;
    const size_t count = (size_t)xnet_hle_register(0x00431bb1u, xnet_start_handler) +
           (size_t)xnet_hle_register(0x00431bc8u, wsa_start_handler) +
           (size_t)xnet_hle_register(0x00431cb6u, xnet_finish_handler) +
           (size_t)xnet_hle_register(0x00431cc3u, wsa_finish_handler);
    if (count) lifecycle_bound = true;
    return count;
}

static uint32_t socket_error(uint32_t error, uint32_t entry)
{
    return lifecycle_result(error, true, entry);
}
static uint32_t socket_object(uint32_t entry)
{
    const uint32_t object = lifecycle_read32(XNET_SINGLETON, entry);
    if (!object || !lifecycle_read16(kernel_guest_add(object, WSA_REF), entry)) return 0u;
    return object;
}
static void socket_zero(uint32_t pointer, size_t bytes, uint32_t entry)
{
    const unsigned char zero[0xe8u] = {0};
    if (bytes > sizeof(zero) || !kernel_guest_write_bytes(pointer, zero, bytes))
        xnet_hle_fatal(entry, "allocated socket storage is unwritable");
}
static uint32_t socket_handler(void *context)
{
    const uint32_t entry = 0x004317b1u;
    const uint32_t object = socket_object(entry);
    if (!object) return socket_error(0x276du, entry);
    const uint32_t family = lifecycle_arg(context, 0u, entry);
    if (family && family != 2u) return socket_error(0x273fu, entry);
    uint32_t type = lifecycle_arg(context, 1u, entry);
    if (type && type != 1u && type != 2u) return socket_error(0x273cu, entry);
    uint32_t protocol = lifecycle_arg(context, 2u, entry);
    if (protocol && protocol != 6u && protocol != 17u && protocol != 254u)
        return socket_error(0x273bu, entry);
    if (!type) type = protocol == 17u || protocol == 254u ? 2u : 1u;
    if (!protocol) protocol = type == 2u ? 17u : 6u;
    const bool tcp = type == 1u;
    if (tcp != (protocol == 6u)) return socket_error(0x273bu, entry);
    const uint32_t previous_irql = kernel_sync_raise_irql(KERNEL_IRQL_DISPATCH);
    uint8_t maximum;
    if (!kernel_guest_read_u8(kernel_guest_add(object, 0xau), &maximum))
        xnet_hle_fatal(entry, "socket capacity is unreadable");
    const uint32_t count_address = kernel_guest_add(object, 0xd34u);
    const uint16_t count = lifecycle_read16(count_address, entry);
    const uint32_t head = kernel_guest_add(object, 0xd38u);
    if (count >= maximum) {
        /* Original 00433B15 may reclaim a closed TCP socket. Detect that state
         * before assigning an allocation error: no guessed errno substitutes
         * for an unimplemented TCP lifetime. This state is not generated by
         * the two admitted local methods. */
        uint32_t node = lifecycle_read32(head, entry);
        for (uint32_t visited = 0u; node != head; visited++) {
            if (visited > count) xnet_hle_fatal(entry, "socket list is cyclic");
            const uint32_t flags = lifecycle_read32(kernel_guest_add(node, 0xcu), entry);
            if ((flags & 0x01000002u) == 0x01000002u)
                xnet_hle_fatal(entry, "closed TCP reclamation needs the lower packet lifetime");
            node = lifecycle_read32(node, entry);
        }
        kernel_sync_restore_irql(previous_irql);
        return socket_error(0x2747u, entry);
    }
    const uint32_t socket = lifecycle.allocate(tcp ? 0xe8u : 0x60u,
                                               tcp ? 0x6154454eu : 0x3854454eu);
    if (!socket) {
        kernel_sync_restore_irql(previous_irql);
        return socket_error(0x2747u, entry);
    }
    socket_zero(socket, tcp ? 0xe8u : 0x60u, entry);
    lifecycle_write32(socket + 8u, 0x2b434f53u, entry);
    lifecycle_write32(socket + 0xcu, (tcp ? 2u : 0u) | 0x400000u |
                                       (protocol == 254u ? 4u : 0u), entry);
    uint8_t send_buffer, receive_buffer;
    if (!kernel_guest_read_u8(kernel_guest_add(object, 0xbu), &send_buffer) ||
        !kernel_guest_read_u8(kernel_guest_add(object, 0xcu), &receive_buffer))
        xnet_hle_fatal(entry, "socket buffer configuration is unreadable");
    lifecycle_write32(socket + 0x58u, (uint32_t)receive_buffer << 10, entry);
    lifecycle_write32(socket + 0x5cu, (uint32_t)send_buffer << 10, entry);
    for (unsigned i=0u; i<3u; i++) {
        const uint32_t offset = i == 0u ? 0x18u : i == 1u ? 0x24u : 0x34u;
        lifecycle_write32(socket + offset, socket + offset, entry);
        lifecycle_write32(socket + offset + 4u, socket + offset, entry);
    }
    if (!kernel_guest_write_u8(socket + 0x12u, 4u))
        xnet_hle_fatal(entry, "socket event type is unwritable");
    if (tcp) {
        uint8_t linger, retransmit;
        if (!kernel_guest_read_u8(kernel_guest_add(object, 0x36u), &linger) ||
            !kernel_guest_read_u8(kernel_guest_add(object, 0x38u), &retransmit))
            xnet_hle_fatal(entry, "TCP configuration is unreadable");
        lifecycle_write16(socket + 0x82u, linger, entry);
        lifecycle_write32(socket + 0xbcu, 0x200u, entry);
        lifecycle_write32(socket + 0xccu, 0x200u, entry);
        lifecycle_write32(socket + 0xb4u, 0xffffu, entry);
        lifecycle_write32(socket + 0xb8u, 0xffffu, entry);
        lifecycle_write32(socket + 0xd8u, (uint32_t)retransmit * 5u, entry);
        lifecycle_write32(socket + 0xdcu, (uint32_t)retransmit * 5u, entry);
        lifecycle_write32(socket + 0x64u, socket + 0x64u, entry);
        lifecycle_write32(socket + 0x68u, socket + 0x64u, entry);
        lifecycle_write32(socket + 0x74u, UINT32_MAX, entry);
        lifecycle_write32(socket + 0x78u, 0x004426aau, entry);
    }
    lifecycle_write16(count_address, (uint16_t)(count + 1u), entry);
    const uint32_t tail = lifecycle_read32(head + 4u, entry);
    lifecycle_write32(socket + 4u, tail, entry);
    lifecycle_write32(socket, head, entry);
    lifecycle_write32(tail, socket, entry);
    lifecycle_write32(head + 4u, socket, entry);
    kernel_sync_restore_irql(previous_irql);
    return socket;
}
static uint32_t socket_acquire(uint32_t socket, uint32_t entry, uint32_t *error)
{
    if (!socket || socket == UINT32_MAX) { *error = 0x2736u; return 0u; }
    if ((socket & 3u) != 0u)
        xnet_hle_fatal(entry, "unaligned socket signature has no native atomic contract");
    _Atomic uint32_t *signature = kernel_guest_at(kernel_guest_add(socket, 8u), 4u);
    if (!signature) xnet_hle_fatal(entry, "socket signature is unreadable");
    uint32_t expected = 0x2b434f53u;
    if (!atomic_compare_exchange_strong(signature, &expected, 0x2a434f53u)) {
        *error = expected == 0x2a434f53u ? 0x2734u : 0x2736u;
        return 0u;
    }
    *error = 0u;
    return socket;
}
static uint32_t ioctl_handler(void *context)
{
    const uint32_t entry = 0x004317d2u;
    const uint32_t object = socket_object(entry);
    if (!object) return socket_error(0x276du, entry);
    uint32_t acquisition_error;
    const uint32_t socket = socket_acquire(lifecycle_arg(context, 0u, entry), entry, &acquisition_error);
    if (!socket) return socket_error(acquisition_error, entry);
    const uint32_t command = lifecycle_arg(context, 1u, entry);
    uint32_t error = 0u;
    if (command == 0x8004667eu) {
        const uint32_t pointer = lifecycle_arg(context, 2u, entry);
        const uint32_t enabled = lifecycle_read32(pointer, entry);
        _Atomic uint32_t *flags = kernel_guest_at(socket + 0xcu, sizeof(uint32_t));
        if (!flags) xnet_hle_fatal(entry, "socket flags are unreadable");
        uint32_t before = atomic_load(flags);
        while (!atomic_compare_exchange_weak(flags, &before,
                   (before & ~0x40000u) | (enabled ? 0x40000u : 0u))) { }

    } else if (command == 0x4004667fu) {
        const uint32_t previous_irql = kernel_sync_raise_irql(KERNEL_IRQL_DISPATCH);
        const uint32_t flags = lifecycle_read32(socket + 0xcu, entry);
        uint32_t available;
        if (flags & 2u) {
            available = lifecycle_read32(socket + 0x2cu, entry);
        } else {
            const uint32_t first = lifecycle_read32(socket + 0x24u, entry);
            available = first == socket + 0x24u ? 0u : lifecycle_read32(first + 8u, entry);
            if (first != socket + 0x24u && !available) available = 1u;
        }
        lifecycle_write32(lifecycle_arg(context, 2u, entry), available, entry);
        kernel_sync_restore_irql(previous_irql);
    } else {
        error = 0x273au;
    }
    lifecycle_write32(socket + 8u, 0x2b434f53u, entry);
    return socket_error(error, entry);
}
static uint32_t bind_handler(void *context)
{
    const uint32_t entry = 0x00431802u;
    const uint32_t object = socket_object(entry);
    if (!object) return socket_error(0x276du, entry);
    uint32_t error;
    const uint32_t socket = socket_acquire(lifecycle_arg(context, 0u, entry), entry, &error);
    if (!socket) return socket_error(error, entry);
    const uint32_t flags = lifecycle_read32(socket + 0xcu, entry);
    if (flags & 0x20u) {
        error = 0x2726u;
    } else {
        const uint32_t address = lifecycle_arg(context, 1u, entry);
        if (lifecycle_read32(kernel_guest_add(address, 4u), entry)) {
            error = 0x2741u;
        } else {
            uint16_t port = lifecycle_read16(kernel_guest_add(address, 2u), entry);
            const uint32_t previous_irql = kernel_sync_raise_irql(KERNEL_IRQL_DISPATCH);
            unsigned tries = 0u;
            if (!port) {
                uint8_t capacity;
                if (!kernel_guest_read_u8(kernel_guest_add(object, 0xau), &capacity))
                    xnet_hle_fatal(entry, "bind capacity is unreadable");
                tries = capacity < 0xe8u ? capacity : 0xe8u;
            }
            const uint32_t head = kernel_guest_add(object, 0xd38u);
            do {
                if (tries) {
                    const uint16_t next = lifecycle_read16(kernel_guest_add(object, 0xd36u), entry);
                    port = (uint16_t)((next << 8) | (next >> 8));
                    const uint16_t after = (uint16_t)(next + 1u);
                    lifecycle_write16(kernel_guest_add(object, 0xd36u),
                                      after <= 0x4e7u ? after : 0x400u, entry);
                    tries--;
                }
                bool conflict = false;
                uint32_t node = lifecycle_read32(head, entry);
                const uint16_t count = lifecycle_read16(kernel_guest_add(object, 0xd34u), entry);
                for (unsigned visited=0u; node != head; visited++) {
                    if (visited > count) xnet_hle_fatal(entry, "bind socket list is cyclic");
                    const uint32_t other = lifecycle_read32(kernel_guest_add(node, 0xcu), entry);
                    if ((other & 0x20u) && !((other ^ flags) & 2u) &&
                        lifecycle_read16(kernel_guest_add(node, 0x4au), entry) == port &&
                        ((other & 0x100000u) || (flags & 0x100000u) || !(flags & 0x80000u))) {
                        conflict = true;
                        break;
                    }
                    node = lifecycle_read32(node, entry);
                }
                if (!conflict) {
                    lifecycle_write16(socket + 0x4au, port, entry);
                    lifecycle_write32(socket + 0xcu, flags | 0x20u, entry);
                    error = 0u;
                    break;
                }
                error = 0x2740u;
            } while (tries);
            kernel_sync_restore_irql(previous_irql);
        }
    }
    lifecycle_write32(socket + 8u, 0x2b434f53u, entry);
    return socket_error(error, entry);
}
static uint32_t close_handler(void *context)
{
    const uint32_t entry = 0x004317bcu;
    const uint32_t object = socket_object(entry);
    if (!object) return socket_error(0x276du, entry);
    uint32_t error;
    const uint32_t socket = socket_acquire(lifecycle_arg(context, 0u, entry), entry, &error);
    if (!socket) return socket_error(error, entry);
    const uint32_t previous_irql = kernel_sync_raise_irql(KERNEL_IRQL_DISPATCH);
    const uint32_t flags = lifecycle_read32(socket + 0xcu, entry);
    /* The local operations generate neither queued packets, accepted children,
     * pending I/O, internal heap records nor connected TCP state. Those lower
     * lifetimes must not be replaced with a false successful close. */
    if ((flags & (1u | 0x40u | 0x800000u)) ||
        lifecycle_read32(socket + 0x20u, entry) || lifecycle_read32(socket + 0x30u, entry) ||
        lifecycle_read32(socket + 0x24u, entry) != socket + 0x24u ||
        lifecycle_read32(socket + 0x34u, entry) != socket + 0x34u ||
        lifecycle_read32(socket + 0x4cu, entry))
        xnet_hle_fatal(entry, "socket close needs lower queued-packet or connected lifetime");
    if (flags & 2u) {
        uint8_t state, children;
        if (!kernel_guest_read_u8(socket + 0x7cu, &state) ||
            !kernel_guest_read_u8(socket + 0x7du, &children))
            xnet_hle_fatal(entry, "TCP lifetime state is unreadable");
        if (state || children || lifecycle_read32(socket + 0x60u, entry) ||
            lifecycle_read32(socket + 0x6cu, entry))
            xnet_hle_fatal(entry, "TCP close needs lower timer/child lifetime");
        lifecycle_write32(socket + 0x74u, UINT32_MAX, entry);
    }
    lifecycle_write32(socket + 0xcu, flags & ~0x400000u, entry);
    const uint32_t previous = lifecycle_read32(socket + 4u, entry);
    const uint32_t next = lifecycle_read32(socket, entry);
    lifecycle_write32(previous, next, entry);
    lifecycle_write32(kernel_guest_add(next, 4u), previous, entry);
    lifecycle_write32(socket + 0x2cu, 0u, entry);
    lifecycle_write32(socket + 0x3cu, 0u, entry);
    lifecycle_write32(socket + 8u, 0x2d636f73u, entry);
    lifecycle.release(socket);
    const uint32_t count = kernel_guest_add(object, 0xd34u);
    lifecycle_write16(count, (uint16_t)(lifecycle_read16(count, entry) - 1u), entry);
    kernel_sync_restore_irql(previous_irql);
    return 0u;
}
static uint32_t receive_handler(void *context, bool from)
{
    const uint32_t entry = from ? 0x00431866u : 0x0043184cu;
    const uint32_t object = socket_object(entry);
    if (!object) return socket_error(0x276du, entry);
    uint32_t error;
    const uint32_t socket = socket_acquire(lifecycle_arg(context, 0u, entry), entry, &error);
    if (!socket) return socket_error(error, entry);
    /* Original recvfrom wrapper reads all six arguments before its lower method.
     * Buffer bytes are untouched on the ordinary empty/nonblocking path. */
    (void)lifecycle_arg(context, 1u, entry);
    (void)lifecycle_arg(context, 2u, entry);
    const uint32_t receive_flags = lifecycle_arg(context, 3u, entry);
    if (from) {
        const uint32_t address = lifecycle_arg(context, 4u, entry);
        const uint32_t length = lifecycle_arg(context, 5u, entry);
        if (address) {
            const unsigned char zero[16] = {0};
            if (!kernel_guest_write_bytes(address, zero, sizeof(zero)))
                xnet_hle_fatal(entry, "recvfrom address is unwritable");
            lifecycle_write32(length, 16u, entry);
            lifecycle_write16(address, 2u, entry);
        }
    }
    const uint32_t flags = lifecycle_read32(socket + 0xcu, entry);
    if (receive_flags)
        xnet_hle_fatal(entry, "nonzero receive flags need the lower receive contract");
    if ((flags & 2u) && !(flags & 0x40u)) {
        error = 0x2749u;
    } else if (!(flags & 2u) && !(flags & 0x20u)) {
        error = 0x2726u;
    } else if ((flags & 0x10u) || (flags & 2u) ||
               lifecycle_read32(socket + 0x24u, entry) != socket + 0x24u) {
        xnet_hle_fatal(entry, "receive needs lower shutdown/TCP/packet queue contract");
    } else if (!(flags & 0x40000u)) {
        xnet_hle_fatal(entry, "blocking receive needs the lower cancellable wait contract");
    } else {
        error = 0x2733u;
    }
    lifecycle_write32(socket + 8u, 0x2b434f53u, entry);
    return socket_error(error, entry);
}
static uint32_t recv_handler(void *context) { return receive_handler(context, false); }
static uint32_t recvfrom_handler(void *context) { return receive_handler(context, true); }
size_t xnet_hle_register_local_sockets(void)
{
    if (!lifecycle.allocate) return 0u;
    const size_t count = (size_t)xnet_hle_register(0x004317b1u, socket_handler) +
           (size_t)xnet_hle_register(0x004317d2u, ioctl_handler) +
           (size_t)xnet_hle_register(0x00431802u, bind_handler) +
           (size_t)xnet_hle_register(0x004317bcu, close_handler) +
           (size_t)xnet_hle_register(0x0043184cu, recv_handler) +
           (size_t)xnet_hle_register(0x00431866u, recvfrom_handler);
    if (count) lifecycle_bound = true;
    return count;
}


bool xnet_hle_seed_digest(uint32_t parameters, uint32_t entropy, uint32_t entropy_bytes,
                          unsigned char digest[20])
{
    if (!digest || entropy_bytes > 512u) return false;
    unsigned char material[20u + 8u + 512u];
    if (!kernel_guest_read_bytes(kernel_guest_add(parameters, 0x10u), material, 20u) ||
        !kernel_guest_read_bytes(kernel_guest_add(parameters, 8u), material + 20u, 8u) ||
        (entropy_bytes && !kernel_guest_read_bytes(entropy, material + 28u, entropy_bytes)))
        return false;
    kernel_crypto_sha1(material, 28u + entropy_bytes, digest);
    return true;
}


bool xnet_hle_seed_state(uint32_t object, uint32_t parameters, uint32_t entropy,
                         uint32_t entropy_bytes)
{
    /* Constructor-only, disjoint inputs. The original ABI is not registered:
     * dependency collection and exceptional/alias cases remain separate work. */
    const uint64_t state_address = (uint64_t)object + 0x88u;
    const uint64_t parameter_address = (uint64_t)parameters + 0x10u;
    if (!object || state_address + KERNEL_RC4_STATE_BYTES > UINT32_MAX ||
        !parameters || parameter_address + 20u > UINT32_MAX || entropy_bytes > 512u)
        return false;
    if (state_address < parameter_address + 20u &&
        parameter_address < state_address + KERNEL_RC4_STATE_BYTES)
        return false;
    unsigned char digest[20], state[KERNEL_RC4_STATE_BYTES], output[20], discard[256] = {0};
    if (!kernel_guest_read_bytes((uint32_t)state_address, state, sizeof(state)) ||
        !kernel_guest_read_bytes((uint32_t)parameter_address, output, sizeof(output)) ||
        !xnet_hle_seed_digest(parameters, entropy, entropy_bytes, digest))
        return false;
    if (!kernel_crypto_rc4_key(state, digest, sizeof(digest)) ||
        !kernel_crypto_rc4_crypt(state, discard, sizeof(discard)) ||
        !kernel_crypto_rc4_crypt(state, output, sizeof(output)))
        return false;
    if (!kernel_guest_write_bytes((uint32_t)state_address, state, sizeof(state))) return false;
    return kernel_guest_write_bytes((uint32_t)parameter_address, output, sizeof(output));
}

bool xnet_hle_collect_seed_state(uint32_t object, uint32_t parameters,
    uint32_t entropy, uint32_t scratch, uint32_t digest,
    const xnet_collector_source *source)
{
    const uint32_t addresses[5] = {object, parameters, entropy, scratch, digest};
    const uint32_t bytes[5] = {0xd50u, 36u, 512u, 512u, 20u};
    for (unsigned i = 0u; i < 5u; ++i) {
        if (!addresses[i] || (uint64_t)addresses[i] + bytes[i] > UINT32_MAX) return false;
        for (unsigned j = 0u; j < i; ++j) {
            if ((uint64_t)addresses[i] < (uint64_t)addresses[j] + bytes[j] &&
                (uint64_t)addresses[j] < (uint64_t)addresses[i] + bytes[i]) return false;
        }
    }
    uint32_t length;
    if (!xnet_collect_entropy(entropy, 512u, scratch, source, &length)) return false;
    unsigned char key[20];
    if (!xnet_hle_seed_digest(parameters, entropy, length, key) ||
        !kernel_guest_write_bytes(digest, key, sizeof(key))) return false;
    /* Original discard encrypts existing guest scratch, including any bytes the
     * collector did not overwrite. No zero stream buffer substitutes for it. */
    return kernel_crypto_rc4_key_guest(object + 0x88u, 20u, digest) == 0u &&
           kernel_crypto_rc4_crypt_guest(object + 0x88u, 256u, entropy) == 0u &&
           kernel_crypto_rc4_crypt_guest(object + 0x88u, 20u, parameters + 0x10u) == 0u;
}

static const unsigned char startup_defaults[76] = {
    0x00, 0x00, 0x0c, 0x08, 0x04, 0x08, 0x40, 0x10, 0x10, 0x04, 0x20, 0x40,
    0x0a, 0x04, 0x01, 0x0a, 0x01, 0x3c, 0x00, 0x40, 0x03, 0x09, 0x04, 0x04,
    0x04, 0x02, 0x0a, 0x3c, 0x05, 0x78, 0x02, 0x0e, 0x05, 0x03, 0x08, 0x09,
    0x05, 0x08, 0x02, 0x14, 0x0d, 0x0a, 0x03, 0x01, 0x05, 0x05, 0x3f, 0x3f,
    0x1e, 0x14, 0x3c, 0x05, 0x03, 0x01, 0x08, 0x05, 0x09, 0x01, 0x04, 0x02,
    0x04, 0x02, 0x05, 0x14, 0x0d, 0x08, 0x04, 0x20, 0x02, 0x40, 0x02, 0x03,
    0x02, 0x03, 0x02, 0x03,
};

static const unsigned char startup_minimum[76] = {
    0x00, 0x00, 0x04, 0x04, 0x01, 0x04, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x04, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x04, 0x01, 0x04, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01,
};

static const unsigned char startup_maximum[76] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xe8, 0x3f, 0x3f, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f, 0x3f,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff,
};

bool xnet_hle_prepare_config(uint32_t object, uint32_t configuration)
{
    /* Original 0043A190 prefix, source tables 444718/444768/4447B8.
     * This does not qualify the remaining constructor or register its ABI. */
    unsigned char output[76], input[76], size = 0;
    uint32_t flags;
    if (!kernel_guest_read_u32(object, &flags) || !kernel_guest_add(object, 79u)) return false;
    memcpy(output, startup_defaults, sizeof(output));
    if (configuration) {
        if (!kernel_guest_read_bytes(configuration, &size, 1u)) return false;
        if (size == 12u || size == 76u) {
            if (!kernel_guest_read_bytes(configuration, input, size)) return false;
            for (unsigned i = 0u; i < size; ++i) {
                const unsigned char value = input[i];
                if (!value) continue;
                output[i] = value < startup_minimum[i] ? startup_minimum[i] :
                            value > startup_maximum[i] ? startup_maximum[i] : value;
            }
        }
    }
    if (!kernel_guest_write_u32(object, flags | 0x20u)) return false;
    return kernel_guest_write_bytes(object + 4u, output, sizeof(output));
}


bool xnet_hle_prepare_pool(uint32_t object, uint32_t pool, uint32_t pool_bytes)
{
    /* Original 0043A288..0043A320. Caller must supply an actually allocated
     * NETe pool. No allocation result, DPC/event or constructor success faked. */
    unsigned char prefix[0x88], first[32], free_block[32], tail[32];
    if (!kernel_guest_read_bytes(object, prefix, sizeof(prefix))) return false;
    const uint32_t pages = prefix[6];
    if (pages < 4u || pages > 232u || pool_bytes != pages * 4096u || !pool ||
        (uint64_t)pool + pool_bytes > UINT32_MAX ||
        (uint64_t)object + 0xd50u > UINT32_MAX ||
        ((uint64_t)pool < (uint64_t)object + 0xd50u &&
         (uint64_t)object < (uint64_t)pool + pool_bytes) ||
        !kernel_guest_range_readable(pool, pool_bytes)) return false;
    const uint32_t units = pool_bytes / 32u - 2u;
    const uint32_t tail_address = pool + pool_bytes - 32u;
    if (!kernel_guest_read_bytes(pool, first, sizeof(first)) ||
        !kernel_guest_read_bytes(pool + 32u, free_block, sizeof(free_block)) ||
        !kernel_guest_read_bytes(tail_address, tail, sizeof(tail))) return false;
    uint32_t flags;
    memcpy(&flags, prefix, sizeof(flags));
    flags |= 0x80u;
    memcpy(prefix, &flags, sizeof(flags));
    memcpy(prefix + 0x50u, &pool, sizeof(pool));
    const uint32_t end = pool + pool_bytes;
    memcpy(prefix + 0x54u, &end, sizeof(end));
    for (unsigned i = 0u; i < 6u; ++i) {
        const uint32_t head = object + 0x58u + i * 8u;
        memcpy(prefix + 0x58u + i * 8u, &head, sizeof(head));
        memcpy(prefix + 0x5cu + i * 8u, &head, sizeof(head));
    }
    const uint32_t head = object + 0x80u, link = pool + 40u;
    memcpy(prefix + 0x80u, &link, sizeof(link));
    memcpy(prefix + 0x84u, &link, sizeof(link));
    first[0] = 0x20u; first[1] = 1u;
    const uint16_t first_tag = 0x6654u, tail_tag = 0x6754u, one = 1u, zero = 0u;
    const uint16_t free_units = (uint16_t)units;
    memcpy(first + 2u, &first_tag, 2u);
    memcpy(first + 4u, &one, 2u); memcpy(first + 6u, &zero, 2u);
    free_block[1] = 0u;
    memcpy(free_block + 2u, &zero, 2u);
    memcpy(free_block + 4u, &free_units, 2u); memcpy(free_block + 6u, &one, 2u);
    memcpy(free_block + 8u, &head, 4u); memcpy(free_block + 12u, &head, 4u);
    tail[0] = 0x20u; tail[1] = 1u;
    memcpy(tail + 2u, &tail_tag, 2u); memcpy(tail + 4u, &one, 2u);
    memcpy(tail + 6u, &free_units, 2u);
    if (!kernel_guest_write_bytes(object, prefix, sizeof(prefix)) ||
        !kernel_guest_write_bytes(pool, first, sizeof(first)) ||
        !kernel_guest_write_bytes(pool + 32u, free_block, sizeof(free_block))) return false;
    return kernel_guest_write_bytes(tail_address, tail, sizeof(tail));
}


bool xnet_hle_decode_config_sector(uint32_t sector, uint32_t payload)
{
    /* Original434068 after a real successful read: caller length is ignored.
     * This is not a read provider; invalid sectors preserve existing payload. */
    if (!sector || !payload || (uint64_t)sector + 512u > UINT32_MAX ||
        (uint64_t)payload + 492u > UINT32_MAX ||
        ((uint64_t)sector < (uint64_t)payload + 492u &&
         (uint64_t)payload < (uint64_t)sector + 512u)) return false;
    unsigned char bytes[512];
    if (!kernel_guest_read_bytes(sector, bytes, sizeof(bytes))) return false;
    uint32_t words[128];
    for (unsigned i = 0u; i < 128u; ++i) {
        const unsigned char *p = bytes + i * 4u;
        words[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    const uint32_t saved_checksum = words[126];
    words[126] = 0u;
    uint32_t sum = 0u, carries = 0u;
    for (unsigned i = 0u; i < 128u; ++i) {
        const uint32_t previous = sum;
        sum += words[i];
        carries += sum < previous;
    }
    const uint32_t previous = sum;
    sum += carries;
    sum += sum < previous;
    if (words[0] != 0x79132568u || words[127] != 0xaa550000u ||
        words[1] < 1u || words[2] < 1u || ~sum != saved_checksum) return false;
    return kernel_guest_write_bytes(payload, bytes + 12u, 492u);
}


bool xnet_hle_prepare_nic_dma(uint32_t object, uint32_t allocation,
                              uint32_t allocation_bytes, uint32_t physical)
{
    unsigned char receive_count, transmit_limit;
    if (!object || !allocation ||
        !kernel_guest_read_bytes(kernel_guest_add(object, 7u), &receive_count, 1u) ||
        !kernel_guest_read_bytes(kernel_guest_add(object, 0x10u), &transmit_limit, 1u)) return false;
    const uint32_t bytes = ((uint32_t)receive_count + 2u) << 11;
    if (allocation_bytes != bytes || (uint64_t)allocation + bytes > UINT32_MAX ||
        (uint64_t)object + 0xd50u > UINT32_MAX ||
        ((uint64_t)object < (uint64_t)allocation + bytes &&
         (uint64_t)allocation < (uint64_t)object + 0xd50u)) return false;
    unsigned char *memory = kernel_guest_at(allocation, bytes);
    if (!memory || !kernel_guest_at(object, 0xd50u)) return false;
    /* Actual43AD4E..43AE2C: only descriptor pages are zeroed. Receive buffers
     * retain allocator contents; physical is the real caller mapping, not a guess. */
    memset(memory, 0, 0x1000u);
    const uint32_t bias = physical - allocation;
    const uint32_t fields[][2] = {
        {0x240u, receive_count}, {0x224u, transmit_limit},
        {0x22cu, allocation + (uint32_t)transmit_limit * 8u},
        {0x21cu, bias}, {0x228u, allocation}, {0x230u, allocation},
        {0x234u, allocation}, {0x244u, allocation + 0x800u},
        {0x24cu, allocation + 0x800u},
        {0x248u, allocation + 0x800u + (uint32_t)receive_count * 8u - 8u}
    };
    for (unsigned i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        if (!kernel_guest_write_u32(object + fields[i][0], fields[i][1])) return false;
    }
    for (uint32_t i = 0u; i < receive_count; ++i) {
        if (!kernel_guest_write_u32(allocation + 0x800u + i * 8u,
                                    physical + 0x1002u + i * 0x800u) ||
            !kernel_guest_write_u32(allocation + 0x804u + i * 8u, 0x800007fdu)) return false;
    }
    return true;
}

static void config_word(unsigned char *bytes, uint32_t word)
{
    for (unsigned i = 0u; i < 4u; ++i) bytes[i] = (unsigned char)(word >> (i * 8u));
}
static bool config_disjoint(uint32_t a, size_t an, uint32_t b, size_t bn)
{
    return a && b && (uint64_t)a + an <= UINT32_MAX &&
        (uint64_t)b + bn <= UINT32_MAX &&
        ((uint64_t)a + an <= b || (uint64_t)b + bn <= a);
}
bool xnet_hle_authenticate_config_payload(uint32_t payload, uint32_t hd_key,
                                         bool *needs_writeback)
{
    unsigned char bytes[492], key[16], digest[20];
    static const unsigned char domain[4] = {0x32u, 0x56u, 0x42u, 0x58u};
    if (!needs_writeback || !config_disjoint(payload, sizeof(bytes), hd_key, sizeof(key)) ||
        !kernel_guest_read_bytes(payload, bytes, sizeof(bytes)) ||
        !kernel_guest_read_bytes(hd_key, key, sizeof(key))) return false;
    kernel_crypto_xc_hmac(key, sizeof(key), bytes + 0x3cu, 432u,
                          domain, sizeof(domain), digest);
    unsigned difference = 0u;
    for (unsigned i = 0u; i < sizeof(digest); ++i) difference |= digest[i] ^ bytes[0x28u + i];
    if (difference) {
        memset(bytes + 0x3cu, 0, 432u);
        config_word(bytes + 0x14u, 0u);
        config_word(bytes + 0x3cu, 0x58425632u);
        config_word(bytes + 0x1e8u, 0x58424350u);
        if (!kernel_guest_write_bytes(payload, bytes, sizeof(bytes))) return false;
    }
    *needs_writeback = difference != 0u;
    return true;
}
bool xnet_hle_encode_config_sector(uint32_t payload, uint32_t hd_key, uint32_t sector)
{
    unsigned char bytes[512] = {0}, key[16], digest[20];
    if (!config_disjoint(payload, 492u, hd_key, sizeof(key)) ||
        !config_disjoint(payload, 492u, sector, sizeof(bytes)) ||
        !config_disjoint(hd_key, sizeof(key), sector, sizeof(bytes)) ||
        !kernel_guest_read_bytes(payload, bytes + 12u, 492u) ||
        !kernel_guest_read_bytes(hd_key, key, sizeof(key))) return false;
    config_word(bytes, 0x79132568u);
    config_word(bytes + 4u, 1u); config_word(bytes + 8u, 1u);
    config_word(bytes + 12u + 0x3cu, 0x58425632u);
    config_word(bytes + 12u + 0x1e8u, 0x58424350u);
    kernel_crypto_xc_hmac(key, sizeof(key), bytes + 12u + 0x3cu, 432u,
                          bytes + 12u + 0x3cu, 4u, digest);
    memcpy(bytes + 12u + 0x28u, digest, sizeof(digest));
    config_word(bytes + 508u, 0xaa550000u);
    uint32_t sum = 0u, carries = 0u;
    for (unsigned i = 0u; i < 128u; ++i) {
        const unsigned char *b = bytes + i * 4u;
        const uint32_t word = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                              ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        const uint32_t previous = sum;
        sum += word; carries += sum < previous;
    }
    const uint32_t previous = sum;
    sum += carries; sum += sum < previous;
    config_word(bytes + 504u, ~sum);
    return kernel_guest_write_bytes(sector, bytes, sizeof(bytes));
}
