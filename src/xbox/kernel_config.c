/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_config.h for the resolved ordinal number, the hand-verified arity
 * across all 12 call sites, the three push idioms that had to be discounted to
 * arrive at 5, and why the settings themselves are not invented.
 */

#include "kernel_config.h"
#include "xnet_eeprom.h"

#include <pthread.h>
#include <string.h>

#include "kernel_call.h"
#include "kernel_hle.h"
#include "nt_status.h"

#define ORD_ExQueryNonVolatileSetting 24u

/*
 * Registry-style type codes, which is what the real export reports through its
 * `Type` out-parameter.
 *
 * UNCONSTRAINED BY ANY MEASUREMENT, and said so rather than implied. The only call
 * site whose use of `Type` is visible (0x0037D3BC) never reads it back -- it reads
 * the 4-byte Value and compares it against 9. So these two values are chosen to be
 * self-consistent with the length we report, not derived from the guest. If a later
 * task finds a site that branches on Type, that site is the evidence and this
 * comment is the thing to replace.
 */
#define CONFIG_TYPE_BINARY 3u
#define CONFIG_TYPE_DWORD 4u

/* How many distinct settings can be stored, and how many distinct indices a run can
 * record. 12 call sites pass 11 distinct literal indices plus whatever the forwarding
 * wrapper at 0x00381B5A is handed at run time, so 32 is ample; an overflow is
 * reported rather than silently dropped. */
#define CONFIG_SETTING_MAX 32u
#define CONFIG_QUERY_LOG_MAX 64u

typedef struct {
    uint32_t index;
    uint32_t length;
    uint8_t bytes[KERNEL_CONFIG_VALUE_MAX];
    bool in_use;
    bool eeprom_source;
    uint32_t source_status;
} config_setting;

/*
 * THE LOCK. Two guest threads run and either can query a setting. Every race here
 * produces a wrong answer rather than a crash: claiming a store slot is a
 * read-modify-write, and the query log is an append. RECURSIVE for the reason
 * kernel_object.c, kernel_pool.c and kernel_hal.c give -- the critical sections call
 * kernel_hle_log(), whose sink is caller-supplied, so a chatty sink must not deadlock.
 */
static pthread_mutex_t config_lock;
static bool config_lock_ready;
static pthread_once_t config_lock_once = PTHREAD_ONCE_INIT;

static void config_lock_init(void)
{
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        return;
    }
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0 &&
        pthread_mutex_init(&config_lock, &attr) == 0) {
        config_lock_ready = true;
    }
    (void)pthread_mutexattr_destroy(&attr);
}

static void lock(void)
{
    (void)pthread_once(&config_lock_once, config_lock_init);
    if (config_lock_ready) {
        (void)pthread_mutex_lock(&config_lock);
    }
}

static void unlock(void)
{
    if (config_lock_ready) {
        (void)pthread_mutex_unlock(&config_lock);
    }
}

static config_setting settings[CONFIG_SETTING_MAX];
static kernel_config_unknown_policy unknown_policy = KERNEL_CONFIG_UNKNOWN_ZEROS;
static unsigned fabricated_count;
static unsigned refused_count;
static unsigned served_count;
static uint32_t queried[CONFIG_QUERY_LOG_MAX];
static unsigned queried_count;
static bool source_keys_available;
static uint8_t source_eeprom_key[16], source_hd_key[16];

/* Callers hold the lock. */
static config_setting *find_locked(uint32_t index)
{
    for (unsigned i = 0u; i < CONFIG_SETTING_MAX; i++) {
        if (settings[i].in_use && settings[i].index == index) {
            return &settings[i];
        }
    }
    return NULL;
}

/* Callers hold the lock. Records an index the first time it is seen, so the log is
 * the set of distinct indices and not a per-call tally -- the question a bring-up run
 * is answering is WHICH settings the title wants, not how often. */
static void record_query_locked(uint32_t index)
{
    for (unsigned i = 0u; i < queried_count && i < CONFIG_QUERY_LOG_MAX; i++) {
        if (queried[i] == index) {
            return;
        }
    }
    if (queried_count < CONFIG_QUERY_LOG_MAX) {
        queried[queried_count] = index;
    }
    queried_count++;
}

void kernel_config_reset(void)
{
    lock();
    memset(settings, 0, sizeof(settings));
    source_keys_available = false;
    memset(source_eeprom_key, 0, sizeof(source_eeprom_key));
    memset(source_hd_key, 0, sizeof(source_hd_key));
    unknown_policy = KERNEL_CONFIG_UNKNOWN_ZEROS;
    fabricated_count = 0u;
    refused_count = 0u;
    served_count = 0u;
    queried_count = 0u;
    memset(queried, 0, sizeof(queried));
    unlock();
}

void kernel_config_set_unknown_policy(kernel_config_unknown_policy policy)
{
    lock();
    unknown_policy = policy;
    unlock();
}

bool kernel_config_set_setting(uint32_t index, const void *bytes, uint32_t length)
{
    if (length > KERNEL_CONFIG_VALUE_MAX) {
        return false;
    }
    if (length > 0u && !bytes) {
        return false;
    }
    lock();
    config_setting *slot = find_locked(index);
    if (!slot) {
        for (unsigned i = 0u; i < CONFIG_SETTING_MAX; i++) {
            if (!settings[i].in_use) {
                slot = &settings[i];
                break;
            }
        }
    }
    if (!slot) {
        unlock();
        return false;
    }
    slot->index = index;
    if (index == 0xffffu) source_keys_available = false;
    slot->eeprom_source = false;
    slot->length = length;
    memset(slot->bytes, 0, sizeof(slot->bytes));
    if (length > 0u) {
        memcpy(slot->bytes, bytes, length);
    }
    slot->in_use = true;
    unlock();
    return true;
}

/* Atomic source-backed pair: full EEPROM and measured factory MAC region.
 * No bytes, encrypted HDD key or other setting is synthesized. */
static bool store_eeprom(const void *bytes, uint32_t length,
                         const uint8_t *eeprom_key, const uint8_t *hd_key)
{
    if (!bytes || length != 256u) return false;
    lock();
    config_setting *full = find_locked(0xffffu), *mac = find_locked(0x101u);
    for (unsigned i = 0u; i < CONFIG_SETTING_MAX; ++i) {
        if (settings[i].in_use) continue;
        if (!full) full = &settings[i];
        else if (!mac && &settings[i] != full) mac = &settings[i];
    }
    if (!full || !mac) { unlock(); return false; }
    full->index = 0xffffu; full->length = 256u;
    memcpy(full->bytes, bytes, 256u);
    full->in_use = full->eeprom_source = true;
    full->source_status = STATUS_SUCCESS;
    mac->index = 0x101u; mac->length = 6u;
    memcpy(mac->bytes, (const unsigned char *)bytes + 64u, 6u);
    mac->in_use = mac->eeprom_source = true;
    uint32_t sum = 0u, carries = 0u;
    const unsigned char *source = bytes;
    for (unsigned i = 0x30u; i < 0x60u; i += 4u) {
        const uint32_t word = (uint32_t)source[i] | ((uint32_t)source[i+1u] << 8) |
            ((uint32_t)source[i+2u] << 16) | ((uint32_t)source[i+3u] << 24);
        const uint32_t previous = sum;
        sum += word; carries += sum < previous;
    }
    const uint32_t previous = sum;
    sum += carries; sum += sum < previous;
    /* Actual80013459/800134D6: invalid factory checksum is DEVICE_DATA_ERROR. */
    mac->source_status = sum == UINT32_MAX ? STATUS_SUCCESS : 0xc000009cu;
    source_keys_available = eeprom_key && hd_key;
    if (source_keys_available) {
        memcpy(source_eeprom_key, eeprom_key, sizeof(source_eeprom_key));
        memcpy(source_hd_key, hd_key, sizeof(source_hd_key));
    } else {
        memset(source_eeprom_key, 0, sizeof(source_eeprom_key));
        memset(source_hd_key, 0, sizeof(source_hd_key));
    }
    unlock();
    return true;
}

bool kernel_config_set_eeprom(const void *bytes, uint32_t length)
{
    return store_eeprom(bytes, length, NULL, NULL);
}
bool kernel_config_set_eeprom_keyed(const void *bytes, uint32_t length,
                                    const void *key, uint32_t key_bytes)
{
    uint8_t hd_key[16];
    if (!bytes || length != 256u || !key || key_bytes != 16u ||
        !xnet_eeprom_derive_hd_key(bytes, key, hd_key)) return false;
    return store_eeprom(bytes, length, key, hd_key);
}
bool kernel_config_eeprom_key(unsigned ordinal, uint8_t output[16])
{
    if (!output || (ordinal != 321u && ordinal != 323u)) return false;
    lock();
    const bool available = source_keys_available;
    if (available) memcpy(output, ordinal == 321u ? source_eeprom_key : source_hd_key, 16u);
    unlock();
    return available;
}

bool kernel_config_eeprom_available(void)
{
    lock();
    const config_setting *stored = find_locked(0xffffu);
    const bool available = stored && stored->eeprom_source;
    unlock();
    return available;
}

unsigned kernel_config_setting_count(void)
{
    unsigned count = 0u;
    lock();
    for (unsigned i = 0u; i < CONFIG_SETTING_MAX; i++) {
        if (settings[i].in_use) {
            count++;
        }
    }
    unlock();
    return count;
}

unsigned kernel_config_fabricated_count(void)
{
    lock();
    const unsigned count = fabricated_count;
    unlock();
    return count;
}

unsigned kernel_config_refused_count(void)
{
    lock();
    const unsigned count = refused_count;
    unlock();
    return count;
}

unsigned kernel_config_served_count(void)
{
    lock();
    const unsigned count = served_count;
    unlock();
    return count;
}

unsigned kernel_config_queried_indices(uint32_t *out, unsigned capacity)
{
    lock();
    const unsigned total = queried_count;
    if (out) {
        for (unsigned i = 0u; i < capacity && i < total && i < CONFIG_QUERY_LOG_MAX; i++) {
            out[i] = queried[i];
        }
    }
    unlock();
    return total;
}

/* Write `length` bytes of guest memory from `source`, or zero-fill when `source` is
 * NULL. Byte at a time through the guest accessor so an unusable address is a
 * refusal rather than a host fault. */
static bool write_value(kernel_guest_ptr destination, const uint8_t *source,
                        uint32_t length)
{
    for (uint32_t i = 0u; i < length; i++) {
        const uint8_t byte = source ? source[i] : 0u;
        if (!kernel_guest_write_u8(destination + i, byte)) {
            return false;
        }
    }
    return true;
}

/* Complex4627 actual800135C3 source-qualified FFFF/101 branches. Caller holds
 * config lock. Checked guest faults remain host refusals, not original SEH. */
static uint32_t query_eeprom(const config_setting *stored, const uint32_t args[5])
{
    if (stored->source_status != STATUS_SUCCESS) return stored->source_status;
    const bool whole = stored->index == 0xffffu;
    if (whole && args[4] && !kernel_guest_write_u32(args[4], 256u))
        return STATUS_INVALID_PARAMETER;
    if (args[3] < stored->length) return KERNEL_CONFIG_STATUS_BUFFER_TOO_SMALL;
    if (!kernel_guest_write_u32(args[1], CONFIG_TYPE_BINARY)) return STATUS_INVALID_PARAMETER;
    if (!whole && args[4] && !kernel_guest_write_u32(args[4], 6u))
        return STATUS_INVALID_PARAMETER;
    if (!whole && ((uint64_t)args[2] + args[3] > UINT32_MAX ||
                   !write_value(args[2], NULL, args[3]))) return STATUS_INVALID_PARAMETER;
    if (!write_value(args[2], stored->bytes, stored->length)) return STATUS_INVALID_PARAMETER;
    served_count++;
    return STATUS_SUCCESS;
}

/*
 * ARITY-OK(24): 5 stack arguments. The measured table has NO row for this ordinal --
 * `callsites.py` classifies it as a DATA export, because every one of the 12 real
 * call sites is a direct `call` to the import jump stub `sub_0038486A` whose body is
 * `jmp [0x4757EC]`, which the measurement cannot see through. So the count comes
 * entirely from a by-hand read of all 12 sites, every one of which pushes exactly 5
 * arguments once two local-reservation `push ecx` pairs, two `push imm`/`pop reg`
 * move idioms and one balanced `push esi`/`pop esi` save are discounted. The 11
 * distinct literal ValueIndex values and the six literal NULL ResultLength arguments
 * pin the argument ORDER as well as the count. Site-by-site table in kernel_config.h.
 */
static uint32_t query_non_volatile_setting(void *context, bool stored_only)
{
    if (!context) {
        kernel_hle_log()("kernel: ExQueryNonVolatileSetting called with no argument "
                         "frame -- the call boundary did not supply one\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;

    uint32_t args[5];
    for (unsigned i = 0u; i < 5u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: ExQueryNonVolatileSetting could not read "
                             "argument %u from the guest stack\n",
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    const uint32_t index = args[0];
    const kernel_guest_ptr type_out = args[1];
    const kernel_guest_ptr value_out = args[2];
    const uint32_t value_length = args[3];
    /* OPTIONAL, and that is measured rather than assumed: six of the twelve call
     * sites pass a literal 0 here. Dereferencing it unconditionally would fault on
     * the guest's very first query. */
    const kernel_guest_ptr result_length_out = args[4];

    lock();
    const config_setting *source_setting = find_locked(index);
    if (source_setting && source_setting->eeprom_source) {
        record_query_locked(source_setting->index);
        const uint32_t result = query_eeprom(source_setting, args);
        unlock();
        return result;
    }
    unlock();

    if (value_out == 0u || value_length == 0u) {
        kernel_hle_log()("kernel: ExQueryNonVolatileSetting(index %#x) has no value "
                         "buffer (pointer %#x, length %u)\n",
                         (unsigned)index, (unsigned)value_out, (unsigned)value_length);
        return STATUS_INVALID_PARAMETER;
    }
    if (value_length > KERNEL_CONFIG_VALUE_MAX) {
        /* Not a buffer-too-small: the GUEST's buffer is larger than anything this
         * module can hold. Reported as its own case so it cannot be mistaken for the
         * guest asking for too little. */
        kernel_hle_log()("kernel: ExQueryNonVolatileSetting(index %#x) asks for %u "
                         "bytes, above the %u this module holds\n",
                         (unsigned)index, (unsigned)value_length,
                         (unsigned)KERNEL_CONFIG_VALUE_MAX);
        return STATUS_INVALID_PARAMETER;
    }

    lock();
    record_query_locked(index);
    const config_setting *stored = find_locked(index);

    uint32_t produced = 0u;
    const uint8_t *source = NULL;
    if (stored) {
        if (stored->length > value_length) {
            kernel_hle_log()("kernel: ExQueryNonVolatileSetting(index %#x) needs %u "
                             "bytes and the guest offered %u\n",
                             (unsigned)index, (unsigned)stored->length,
                             (unsigned)value_length);
            unlock();
            return KERNEL_CONFIG_STATUS_BUFFER_TOO_SMALL;
        }
        produced = stored->length;
        source = stored->bytes;
        served_count++;
    } else if (stored_only || unknown_policy == KERNEL_CONFIG_UNKNOWN_FAIL) {
        refused_count++;
        kernel_hle_log()("kernel: ExQueryNonVolatileSetting(index %#x) REFUSED -- no "
                         "value is held and %s\n",
                         (unsigned)index, stored_only ? "this query requires stored bytes" :
                                                      "the policy is FAIL");
        unlock();
        return KERNEL_CONFIG_STATUS_OBJECT_NAME_NOT_FOUND;
    } else {
        /* Loud on purpose. The whole length is zero-filled rather than a prefix,
         * because a partially-written buffer would leave the guest reading its own
         * uninitialised stack for the rest -- which is worse than a known-wrong zero
         * and far harder to recognise later. */
        produced = value_length;
        source = NULL;
        fabricated_count++;
        kernel_hle_log()("kernel: ExQueryNonVolatileSetting(index %#x) -> %u zero "
                         "bytes, FABRICATED (no EEPROM data; the guest's "
                         "configuration from here on is ours, not the console's)\n",
                         (unsigned)index, (unsigned)value_length);
    }
    unlock();

    /*
     * Written in a FIXED ORDER -- Value, then Type, then ResultLength -- and it is
     * stated because the arguments can alias: at 0x00441690 the same register is
     * handed in as Type, Value and ResultLength. With aliasing the last write wins,
     * so the order has to be a decision rather than an accident of how the code
     * happens to be arranged.
     */
    if (!write_value(value_out, source, produced)) {
        kernel_hle_log()("kernel: ExQueryNonVolatileSetting(index %#x) could not write "
                         "%u bytes to %#x\n",
                         (unsigned)index, (unsigned)produced, (unsigned)value_out);
        return STATUS_INVALID_PARAMETER;
    }
    if (type_out != 0u) {
        const uint32_t type = (produced == 4u) ? CONFIG_TYPE_DWORD : CONFIG_TYPE_BINARY;
        if (!kernel_guest_write_u32(type_out, type)) {
            kernel_hle_log()("kernel: ExQueryNonVolatileSetting(index %#x) could not "
                             "write Type to %#x\n",
                             (unsigned)index, (unsigned)type_out);
            return STATUS_INVALID_PARAMETER;
        }
    }
    if (result_length_out != 0u &&
        !kernel_guest_write_u32(result_length_out, produced)) {
        kernel_hle_log()("kernel: ExQueryNonVolatileSetting(index %#x) could not write "
                         "ResultLength to %#x\n",
                         (unsigned)index, (unsigned)result_length_out);
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

/* Collector requests must never consume the default unknown-value fabrication.
 * This is the same kernel store/query operation with a per-request strict policy;
 * no global policy change or temporary unlock/switch races. */
uint32_t kernel_config_query_stored(void *context)
{
    return query_non_volatile_setting(context, true);
}
static uint32_t hle_ex_query_non_volatile_setting(void *context)
{
    return query_non_volatile_setting(context, false);
}

unsigned kernel_config_register(void)
{
    static const struct {
        unsigned ordinal;
        kernel_fn handler;
    } bindings[] = {
        {ORD_ExQueryNonVolatileSetting, hle_ex_query_non_volatile_setting},
    };

    unsigned bound = 0u;
    for (size_t i = 0u; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        if (kernel_hle_register(bindings[i].ordinal, bindings[i].handler)) {
            bound++;
        }
    }
    return bound;
}
