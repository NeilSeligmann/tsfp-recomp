/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mu_device.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel_call.h"
#include "kernel_file.h"
#include "xinput_hle.h"
#include "xinput_devices.h"

typedef struct {
    uint8_t *image;
    size_t size;
    char *path;
    bool mounted;
    unsigned device_id;
    char device_path[40], drive_link[16];
    bool guest_backed;
    fatx_volume volume;
} mu_slot;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static mu_slot slots[MU_PORTS * MU_SLOTS];
static uint32_t mounted_mask;
static bool guest_filesystem;
static unsigned free_ids[8] = {0,1,2,3,4,5,6,7}, free_id_count = 8;
static mu_fatal_fn fatal_handler;

static void refuse(uint32_t entry, const char *message) __attribute__((noreturn));
static void refuse(uint32_t entry, const char *message)
{
    xinput_hle_log()("mu: %#x refused: %s\n", entry, message);
    (void)pthread_mutex_unlock(&lock);
    if (fatal_handler != NULL) fatal_handler(entry, message);
    abort();
}
void mu_set_fatal(mu_fatal_fn handler) { fatal_handler = handler; }

/* MEASURED 0046D7E6..0046D7FA: letter = (int8)((port+0x23)*2+slot), bit = 1 << (letter-0x46), device table
 * index = (port*2+slot)*3 entries of 12 bytes at [0x771364]. So slot>=2 aliases the next port's slot. */
static int index_of(uint32_t port, uint32_t slot)
{
    const uint32_t index = port * 2u + slot;
    return port < 0x1000u && slot < 0x1000u && index < MU_PORTS * MU_SLOTS ? (int)index : -1;
}
char mu_drive_letter(uint32_t port, uint32_t slot)
{
    const int index = index_of(port, slot);
    return index < 0 ? 0 : (char)(0x46 + index);
}
static void mirror_mask(void)
{
    uint8_t *at = kernel_guest_at(MU_MASK_ADDRESS, 4u);
    if (at != NULL) memcpy(at, &mounted_mask, 4u);
}
static void free_slot(mu_slot *slot)
{
    free(slot->image); free(slot->path);
    memset(slot, 0, sizeof(*slot));
}
static void locked_mu_reset(void)
{
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < MU_PORTS * MU_SLOTS; i++) {
        if (slots[i].guest_backed) {
            (void)kernel_file_remove_symlink(slots[i].drive_link);
            (void)kernel_file_unmount_fatx(slots[i].device_path);
        }
        free_slot(&slots[i]); free_ids[i] = i;
    }
    free_id_count = 8;
    mounted_mask = 0u;
    mirror_mask(); /* Reset ownership and its mapped guest mirror under the same lock. */
    pthread_mutex_unlock(&lock);
}
static mu_slot *slot_at(unsigned port, unsigned slot)
{
    const int index = port < MU_PORTS && slot < MU_SLOTS ? index_of(port, slot) : -1;
    return index < 0 ? NULL : &slots[index];
}
static bool locked_mu_attach_blank(unsigned port, unsigned slot, size_t size, bool format, uint32_t volume_id)
{
    pthread_mutex_lock(&lock);
    mu_slot *s = slot_at(port, slot);
    bool ok = s != NULL && s->image == NULL && size >= FATX_MIN_IMAGE && size % FATX_SECTOR_BYTES == 0u;
    if (ok) {
        s->image = calloc(1u, size);
        ok = s->image != NULL && (!format || fatx_format(s->image, size, volume_id) == FATX_OK);
        if (ok) s->size = size; else free_slot(s);
    }
    pthread_mutex_unlock(&lock);
    return ok;
}
static bool locked_mu_attach_image(unsigned port, unsigned slot, const uint8_t *image, size_t size)
{
    pthread_mutex_lock(&lock);
    mu_slot *s = slot_at(port, slot);
    bool ok = s != NULL && s->image == NULL && image != NULL && size >= FATX_MIN_IMAGE && size % FATX_SECTOR_BYTES == 0u;
    if (ok) {
        s->image = malloc(size);
        ok = s->image != NULL;
        if (ok) { memcpy(s->image, image, size); s->size = size; }
    }
    pthread_mutex_unlock(&lock);
    return ok;
}
static bool locked_mu_attach_file(unsigned port, unsigned slot, const char *path, size_t create_size)
{
    if (path == NULL) return false;
    FILE *f = fopen(path, "rb");
    bool ok = false;
    if (f != NULL) {
        long end = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
        uint8_t *buf = end > 0 ? malloc((size_t)end) : NULL;
        if (buf != NULL && fseek(f, 0, SEEK_SET) == 0 && fread(buf, 1u, (size_t)end, f) == (size_t)end)
            ok = locked_mu_attach_image(port, slot, buf, (size_t)end);
        free(buf); fclose(f);
    } else if (create_size != 0u) {
        ok = locked_mu_attach_blank(port, slot, create_size, true, 0x4D550000u | port << 8 | slot);
    }
    if (!ok) return false;
    pthread_mutex_lock(&lock);
    mu_slot *s = slot_at(port, slot);
    s->path = strdup(path);
    ok = s->path != NULL;
    if (!ok) free_slot(s);
    pthread_mutex_unlock(&lock);
    return ok;
}
static bool flush(mu_slot *s)
{
    if (s->path == NULL) return true;
    FILE *f = fopen(s->path, "wb");
    if (f == NULL) return false;
    const bool ok = fwrite(s->image, 1u, s->size, f) == s->size;
    return fclose(f) == 0 && ok;
}
static bool flush_guest_image(void *opaque)
{
    pthread_mutex_lock(&lock); const bool ok = flush(opaque); pthread_mutex_unlock(&lock); return ok;
}
static bool locked_mu_detach(unsigned port, unsigned slot)
{
    pthread_mutex_lock(&lock);
    mu_slot *s = slot_at(port, slot);
    const bool ok = s != NULL && s->image != NULL && !s->mounted;
    if (ok) free_slot(s);
    pthread_mutex_unlock(&lock);
    return ok;
}
bool mu_attached(unsigned port, unsigned slot)
{
    pthread_mutex_lock(&lock);
    mu_slot *s = slot_at(port, slot);
    const bool ok = s != NULL && s->image != NULL;
    pthread_mutex_unlock(&lock);
    return ok;
}
uint8_t *mu_image(unsigned port, unsigned slot, size_t *size)
{
    pthread_mutex_lock(&lock);
    mu_slot *s = slot_at(port, slot);
    uint8_t *image = s != NULL ? s->image : NULL;
    if (size != NULL) *size = image != NULL ? s->size : 0u;
    pthread_mutex_unlock(&lock);
    return image;
}
fatx_volume *mu_volume(unsigned port, unsigned slot)
{
    pthread_mutex_lock(&lock);
    mu_slot *s = slot_at(port, slot);
    fatx_volume *v = s != NULL && s->mounted ? &s->volume : NULL;
    pthread_mutex_unlock(&lock);
    return v;
}
uint32_t mu_mounted_mask(void)
{
    pthread_mutex_lock(&lock);
    const uint32_t mask = mounted_mask;
    pthread_mutex_unlock(&lock);
    return mask;
}

static uint32_t locked_mount(uint32_t port, uint32_t slot, bool drive_wanted, char *drive)
{
    const int index = index_of(port, slot);
    if (index < 0) refuse(MU_ENTRY_MOUNT, "port/slot outside the 8-entry device table: the original does not range-check and would index past it");
    const char letter = (char)(0x46 + index);
    mu_slot *s = &slots[index];
    if (drive_wanted) *drive = 0;                            /* 0046D7D4 clears before locking */
    if ((mounted_mask & (1u << index)) != 0u) {              /* 0046D7FC .. 0046D813 */
        if (drive_wanted) *drive = letter;
        return MU_ERROR_ALREADY_ASSIGNED;
    }
    if (s->image == NULL) return MU_ERROR_DEVICE_NOT_CONNECTED;   /* 0046DF1.. 0xC000009D -> 0x48F */
    if (fatx_open(&s->volume, s->image, s->size) != FATX_OK) return MU_ERROR_UNRECOGNIZED_VOLUME;
    if (guest_filesystem) {
        uint32_t certificate, title_id;
        if (!kernel_guest_read_u32(0x00010118u, &certificate) || certificate > UINT32_MAX - 8u ||
            !kernel_guest_read_u32(certificate + 8u, &title_id)) return 0x57u;
        if (free_id_count == 0u) return 8u;
        char title[9], target[64];
        snprintf(title, sizeof(title), "%08x", title_id);
        fatx_status result = fatx_mkdir_path(&s->volume, title);
        bool directory; uint32_t size;
        if (result == FATX_E_EXISTS) result = fatx_stat_path(&s->volume, title, &directory, &size);
        else directory = result == FATX_OK;
        if (result != FATX_OK || !directory) return MU_ERROR_UNRECOGNIZED_VOLUME;
        s->device_id = free_ids[free_id_count - 1u];
        snprintf(s->device_path, sizeof(s->device_path), "\\Device\\MU_%x", s->device_id);
        snprintf(s->drive_link, sizeof(s->drive_link), "\\??\\%c:", letter);
        snprintf(target, sizeof(target), "%s\\%s", s->device_path, title);
        if (!kernel_file_mount_fatx(s->device_path, &s->volume, flush_guest_image, s)) return 8u;
        if (!kernel_file_add_symlink(s->drive_link, target)) {
            (void)kernel_file_unmount_fatx(s->device_path); return 8u;
        }
        free_id_count--; s->guest_backed = true;
    }
    s->mounted = true;
    mounted_mask |= 1u << index;
    mirror_mask();
    if (drive_wanted) *drive = letter;
    return 0u;
}
static uint32_t file_locked_mount(uint32_t port, uint32_t slot, bool drive_wanted, char *drive)
{
    pthread_mutex_lock(&lock);
    char scratch = 0;
    const uint32_t result = locked_mount(port, slot, drive_wanted, drive_wanted ? drive : &scratch);
    pthread_mutex_unlock(&lock);
    return result;
}
static uint32_t file_locked_unmount(uint32_t port, uint32_t slot)
{
    pthread_mutex_lock(&lock);
    const int index = index_of(port, slot);
    if (index < 0) refuse(MU_ENTRY_UNMOUNT, "port/slot outside the 8-entry device table: the original does not range-check");
    if ((mounted_mask & (1u << index)) == 0u) { pthread_mutex_unlock(&lock); return MU_ERROR_INVALID_DRIVE; }
    mu_slot *s = &slots[index];
    const bool flushed = flush(s);
    if (s->guest_backed) {
        (void)kernel_file_remove_symlink(s->drive_link);
        (void)kernel_file_unmount_fatx(s->device_path);
        free_ids[free_id_count++] = s->device_id; s->guest_backed = false;
    }
    s->mounted = false;
    memset(&s->volume, 0, sizeof(s->volume));
    mounted_mask &= ~(1u << index);
    mirror_mask();
    pthread_mutex_unlock(&lock);
    return flushed ? 0u : 0x1Du; /* ERROR_WRITE_FAULT: host flush failure, FABRICATED mapping */
}

typedef struct { uint32_t port, slot, result; bool wanted, unmount; char *drive; } mount_request;
static void mount_job(void *opaque)
{
    mount_request *r = opaque;
    r->result = r->unmount ? file_locked_unmount(r->port, r->slot) :
        file_locked_mount(r->port, r->slot, r->wanted, r->drive);
}
uint32_t mu_xmount(uint32_t port, uint32_t slot, bool wanted, char *drive)
{
    pthread_mutex_lock(&lock);
    if (index_of(port, slot) < 0) {
        refuse(MU_ENTRY_MOUNT, "port/slot outside the 8-entry device table: original unchecked access");
    }
    if (!wanted && (mounted_mask & (1u << index_of(port, slot))) != 0u)
        refuse(MU_ENTRY_MOUNT, "duplicate mount NULL drive output faults at original 0046D80B");
    pthread_mutex_unlock(&lock);
    mount_request r = {.port=port, .slot=slot, .wanted=wanted, .drive=drive};
    kernel_file_run_locked(mount_job, &r); return r.result;
}
uint32_t mu_xunmount(uint32_t port, uint32_t slot)
{
    /* Fatal original-domain guards precede the file lock: a fatal catcher must not
     * strand an acquired filesystem lock. Successful lifecycle work then locks file→MU. */
    pthread_mutex_lock(&lock);
    const int index = index_of(port, slot);
    if (index < 0) refuse(MU_ENTRY_UNMOUNT, "port/slot outside the original device table");
    const uint8_t *title = kernel_guest_at(MU_TITLE_DRIVE_ADDRESS, 1u);
    if ((mounted_mask & (1u << index)) != 0u && title != NULL && *title == (uint8_t)(0x46 + index))
        refuse(MU_ENTRY_UNMOUNT, "unmounting the title's own MU drive calls 0037DB94(0x58) in the original, not modelled");
    pthread_mutex_unlock(&lock);
    mount_request r = {.port=port, .slot=slot, .unmount=true};
    kernel_file_run_locked(mount_job, &r); return r.result;
}
/* Startup opt-in only: live mounts cannot change their namespace contract. */
bool mu_enable_guest_filesystem(bool enabled)
{
    pthread_mutex_lock(&lock);
    const bool ok = mounted_mask == 0u;
    if (ok) guest_filesystem = enabled;
    pthread_mutex_unlock(&lock); return ok;
}
static uint32_t argument(void *context, uint32_t entry, unsigned index)
{
    uint32_t value;
    if (!kernel_frame_arg(context, index, &value)) refuse(entry, "unreadable stack argument");
    return value;
}
static uint32_t mount_handler(void *context)
{
    const uint32_t port = argument(context, MU_ENTRY_MOUNT, 0u), slot = argument(context, MU_ENTRY_MOUNT, 1u);
    const uint32_t out = argument(context, MU_ENTRY_MOUNT, 2u);
    uint8_t *at = NULL;
    if (out != 0u) {
        at = kernel_guest_at(out, 1u);
        if (at == NULL) refuse(MU_ENTRY_MOUNT, "drive output pointer is not mapped (the original faults)");
        *at = 0u; /* Original 0046D7D4 clears before locking/device access. */
    }
    char drive = 0;
    const uint32_t result = mu_xmount(port, slot, out != 0u, &drive);
    if (at != NULL) *at = (uint8_t)drive;
    return result;
}
static uint32_t unmount_handler(void *context)
{ return mu_xunmount(argument(context, MU_ENTRY_UNMOUNT, 0u), argument(context, MU_ENTRY_UNMOUNT, 1u)); }
size_t mu_register(void)
{
    size_t count = 0u;
    count += xinput_hle_register(MU_ENTRY_MOUNT, mount_handler) ? 1u : 0u;
    count += xinput_hle_register(MU_ENTRY_UNMOUNT, unmount_handler) ? 1u : 0u;
    return count;
}

/* Serialize presence notification and image mutation as one ordered lifecycle.
 * Mount/unmount only affect drive ownership, never physical presence. */
typedef struct {
    unsigned operation, port, slot;
    size_t size;
    bool format, result;
    uint32_t volume_id;
    const uint8_t *image;
    const char *path;
} mu_lifecycle;
static void file_locked_lifecycle_job(void *opaque)
{
    mu_lifecycle *r = opaque;
    switch (r->operation) {
    case 0u: locked_mu_reset(); r->result = true; break;
    case 1u: r->result = locked_mu_attach_blank(r->port, r->slot, r->size, r->format, r->volume_id); break;
    case 2u: r->result = locked_mu_attach_image(r->port, r->slot, r->image, r->size); break;
    case 3u: r->result = locked_mu_attach_file(r->port, r->slot, r->path, r->size); break;
    case 4u: r->result = locked_mu_detach(r->port, r->slot); break;
    default: abort();
    }
    uint32_t mask = 0u;
    pthread_mutex_lock(&lock);
    for (unsigned port = 0u; port < MU_PORTS; port++)
        for (unsigned slot = 0u; slot < MU_SLOTS; slot++)
            if (slots[port * MU_SLOTS + slot].image != NULL) mask |= 1u << (port + 16u * slot);
    pthread_mutex_unlock(&lock);
    xinput_devices_set_mu_mask(mask);
}
static void lifecycle_job(void *opaque)
{
    kernel_file_run_locked(file_locked_lifecycle_job, opaque);
}
void mu_reset(void)
{
    mu_lifecycle r = {0};
    xinput_devices_run_locked(lifecycle_job, &r);
}
bool mu_attach_blank(unsigned port, unsigned slot, size_t size, bool format, uint32_t volume_id)
{
    mu_lifecycle r = {.operation = 1u, .port = port, .slot = slot, .size = size, .format = format, .volume_id = volume_id};
    xinput_devices_run_locked(lifecycle_job, &r);
    return r.result;
}
bool mu_attach_image(unsigned port, unsigned slot, const uint8_t *image, size_t size)
{
    mu_lifecycle r = {.operation = 2u, .port = port, .slot = slot, .size = size, .image = image};
    xinput_devices_run_locked(lifecycle_job, &r);
    return r.result;
}
bool mu_attach_file(unsigned port, unsigned slot, const char *path, size_t create_size)
{
    mu_lifecycle r = {.operation = 3u, .port = port, .slot = slot, .size = create_size, .path = path};
    xinput_devices_run_locked(lifecycle_job, &r);
    return r.result;
}
bool mu_detach(unsigned port, unsigned slot)
{
    mu_lifecycle r = {.operation = 4u, .port = port, .slot = slot};
    xinput_devices_run_locked(lifecycle_job, &r);
    return r.result;
}
