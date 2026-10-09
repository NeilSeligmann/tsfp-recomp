/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_XBOX_XNET_HLE_H
#define TSFP_XBOX_XNET_HLE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xnet_collector.h"

/* Mirrors xdk_surface_entry from the generated, gitignored src/xbox/xdk_surface.h, declared
 * locally so this header compiles in a fresh clone where that file has not been generated
 * (T255). Layout is pinned against the real type in xnet_hle.c whenever it exists. */
typedef struct {
    uint32_t address;
    const char *section;
    const char *name;
    uint32_t sites;
} xnet_xdk_row;

typedef uint32_t (*xnet_fn)(void *context);
typedef int (*xnet_log_fn)(const char *format, ...);
typedef void (*xnet_fatal_fn)(uint32_t address, const char *message);
typedef struct {
    uint32_t address;
    const char *name;
    uint32_t sites;
} xnet_surface_entry;
typedef enum { XNET_ENTRY_STUB = 0, XNET_ENTRY_IMPLEMENTED } xnet_entry_state;
typedef struct {
    uint32_t address;
    const char *name;
    uint32_t sites;
    xnet_fn handler;
    xnet_entry_state state;
    uint32_t default_return;
    uint64_t call_count;
    bool reported;
} xnet_entry;
/* Rows are copied; names are borrowed and must outlive the adopted registry.
 * Invalid replacement leaves the old registry intact. Initialization is session setup. */
bool xnet_hle_init(const xnet_surface_entry *rows, size_t count);
bool xnet_hle_adopt(const xnet_xdk_row *rows, size_t count);
void xnet_hle_shutdown(void);
bool xnet_hle_register(uint32_t address, xnet_fn handler);
bool xnet_hle_set_default_return(uint32_t address, uint32_t value);
const xnet_entry *xnet_hle_entry(uint32_t address);
/* Missing calls report once and return the configured default; the production
 * thunk enforces stop-on-missing before calling this registry. */
uint32_t xnet_hle_call(uint32_t address, void *context);
size_t xnet_hle_count(void);
size_t xnet_hle_implemented_count(void);
uint64_t xnet_hle_unknown_count(void);
void xnet_hle_report_backlog(void);
void xnet_hle_set_log(xnet_log_fn printer);
xnet_log_fn xnet_hle_log(void);
void xnet_hle_set_fatal(xnet_fatal_fn handler);
void xnet_hle_fatal(uint32_t address, const char *format, ...)
    __attribute__((format(printf, 2, 3), noreturn));

/* T1091: original lifecycle boundary. This is deliberately not registered by
 * default: a provider must execute the complete lower constructor, including
 * RC4 and NIC state, before a new object is usable. Existing singleton reference
 * transitions are independent of the NIC. All addresses remain guest pointers. */
typedef struct {
    uint32_t (*allocate)(uint32_t bytes, uint32_t tag);
    void (*release)(uint32_t object);
    /* Original constructor stages after the outer allocation/list setup.
     * Returns the original DOS result, not a host errno. A failure must leave
     * enough state for destroy; no reference is acquired on failure. */
    uint32_t (*initialize)(uint32_t object, uint32_t params);
    void (*close_sockets)(uint32_t object);
    /* final_cleanup distinguishes 434308(1,1) + handle release on final
     * cleanup from the constructor-failure unwind. */
    void (*destroy)(uint32_t object, bool final_cleanup);
    bool (*set_last_error)(uint32_t error);
} xnet_lifecycle_provider;

/* Setup only: changing/clearing a registered provider is refused. Registry
 * shutdown clears it; caller owns cleanup of any remaining guest resources. */
bool xnet_hle_set_lifecycle_provider(const xnet_lifecycle_provider *provider);
/* Four original wrappers, admitted only with the complete provider above. */
size_t xnet_hle_register_lifecycle(void);
/* Constructor prefix is separately usable by a provider. It initializes the
 * measured outer D50-byte object and subobject vtables only, never claims NIC
 * initialization or publishes the singleton. */
bool xnet_hle_prepare_object(uint32_t object);

/* Original local socket allocation/ioctl/bind state, fresh close and
 * empty nonblocking receive. Like lifecycle admission,
 * this requires the complete provider; it sends no packets. Packet queues and
 * connected TCP teardown remain separate lower-stack contracts, not host errno fallbacks. */
size_t xnet_hle_register_local_sockets(void);

/* Constructor-only digest from 00439C30/00439897: parameter bytes +10h (20),
 * +8h (8), then the caller-provided entropy span (at most the original collector's
 * 512-byte capacity). Does not create entropy, initialize RC4 or publish state.
 * Failure leaves the host digest output unchanged. */
bool xnet_hle_seed_digest(uint32_t parameters, uint32_t entropy, uint32_t entropy_bytes,
                          unsigned char digest[20]);

/* Bounded 00439C30 seed transition with caller-supplied entropy: SHA composition,
 * RC4 key at object+88h, discard 256 stream bytes, XOR 20 into parameters+10h.
 * No singleton/entropy collector/NIC admission. Requires disjoint state/output;
 * pointer protection faults retain the existing guest fault boundary. */
bool xnet_hle_seed_state(uint32_t object, uint32_t parameters, uint32_t entropy,
                         uint32_t entropy_bytes);

/* Complete bounded00439C30 data transition with actual collector callbacks.
 * Enclosing parameters36 bytes, entropy512, scratch512 and digest20 are genuine
 * disjoint guest storage, also disjoint from the object. Prior bytes are retained.
 * Performs actual guest-buffer RC4 discard256 then parameter XOR20; no source
 * defaults, singleton, NIC admission or original exceptional SEH claim. */
bool xnet_hle_collect_seed_state(uint32_t object, uint32_t parameters,
    uint32_t entropy, uint32_t scratch, uint32_t digest,
    const xnet_collector_source *source);

/* 0043A190 configuration prefix: default 76 bytes, size 12/76 overlays with
 * original zero/default and min/max clamps, flags OR20h. Configuration is the
 * config block itself (not the surrounding lower-constructor argument struct).
 * Separate, unregistered prefix; no entropy/resources/NIC success implied. */
bool xnet_hle_prepare_config(uint32_t object, uint32_t configuration);

/* 0043A288 pool boundary/free-list initialization for a real caller-owned NETe
 * allocation matching clamped config pages. Preserves untouched allocation bytes.
 * Does not allocate, collect entropy, install DPC/event or publish a singleton. */
bool xnet_hle_prepare_pool(uint32_t object, uint32_t pool, uint32_t pool_bytes);

/* Original434068 sector validation/copy after successful actual read. Requires
 * disjoint sector512 and payload492; rejects invalid sectors without writes.
 * No read/provider/HMAC/default configuration or source provenance is implied. */
bool xnet_hle_decode_config_sector(uint32_t sector, uint32_t payload);

/* Original43426E post-read authentication/defaults. Preserves genuine existing
 * first20 bytes even on invalid authentication. Caller owns payload492/key16.
 * Host bool reports invocation only; needs_writeback is not successful disk IO. */
bool xnet_hle_authenticate_config_payload(uint32_t payload, uint32_t hd_key,
                                         bool *needs_writeback);
/* Original4341C2/43411F private-copy signing/sector encoding. Disjoint genuine
 * caller spans; caller payload remains unchanged; no write/provider implied. */
bool xnet_hle_encode_config_sector(uint32_t payload, uint32_t hd_key, uint32_t sector);

/* Original43AD4E..43AE2C NIC DMA ring setup. Requires an actual caller-owned
 * contiguous allocation of (config[7]+2)*2048 bytes and its physical mapping.
 * Zeroes descriptor pages only; preserves receive-buffer contents. No MMIO,
 * interrupt/PHY activation, simulated physical mapping or NIC success implied. */
bool xnet_hle_prepare_nic_dma(uint32_t object, uint32_t allocation,
                              uint32_t allocation_bytes, uint32_t physical);

#endif
