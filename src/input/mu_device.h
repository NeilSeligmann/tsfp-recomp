/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MU_DEVICE_H
#define MU_DEVICE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "mu_fatx.h"

/* Memory-unit block device and XMountMUA / XUnmountMU (T1085).
 * Original routes: XMountMUA 0046D7C4 (device 0046DEE5, FATX mount 00380E6C), XUnmountMU 0046D8F6.
 * Contract and labels: docs/t1085-mu-fatx.md. No MU is attached by default, so the default answers are the
 * original's no-device results. Attachment is explicit host policy (FABRICATED presence). */
#define MU_PORTS 4u
#define MU_SLOTS 2u
#define MU_ENTRY_MOUNT 0x0046D7C4u
#define MU_ENTRY_UNMOUNT 0x0046D8F6u
#define MU_MASK_ADDRESS 0x0076E348u
#define MU_TITLE_DRIVE_ADDRESS 0x0076E344u

#define MU_ERROR_INVALID_DRIVE 0x0Fu
#define MU_ERROR_ALREADY_ASSIGNED 0x55u
#define MU_ERROR_DEVICE_NOT_CONNECTED 0x48Fu
#define MU_ERROR_UNRECOGNIZED_VOLUME 0x3EDu

typedef void (*mu_fatal_fn)(uint32_t entry, const char *message);

void mu_reset(void);
/** Explicit host FATX guest route, startup only; no original kernel status claim. */
bool mu_enable_guest_filesystem(bool enabled);
void mu_set_fatal(mu_fatal_fn handler);
/** Plug a blank (optionally FATX formatted) MU of `size` bytes. False if occupied, bad slot or bad size. */
bool mu_attach_blank(unsigned port, unsigned slot, size_t size, bool format, uint32_t volume_id);
/** Plug a copy of `image`. */
bool mu_attach_image(unsigned port, unsigned slot, const uint8_t *image, size_t size);
/** Plug a host file as the MU image; flushed back on unmount. Created blank-formatted if absent and `create_size`!=0. */
bool mu_attach_file(unsigned port, unsigned slot, const char *path, size_t create_size);
/** Unplug. Refused (false) while mounted. */
bool mu_detach(unsigned port, unsigned slot);
bool mu_attached(unsigned port, unsigned slot);
/** Raw block image, or NULL. Writes through it bypass the mount. */
uint8_t *mu_image(unsigned port, unsigned slot, size_t *size);
/** Open volume of a MOUNTED unit, or NULL when not mounted. */
fatx_volume *mu_volume(unsigned port, unsigned slot);
uint32_t mu_mounted_mask(void);
/** Drive letter for port/slot, or 0 when the original's index (port*2+slot) is outside the 8-entry table. */
char mu_drive_letter(uint32_t port, uint32_t slot);

/** Host-side XMountMUA: returns the Win32 error. `*drive` is 0 or the letter, as the guest byte would be. */
uint32_t mu_xmount(uint32_t port, uint32_t slot, bool drive_wanted, char *drive);
/** Host-side XUnmountMU: returns the Win32 error. */
uint32_t mu_xunmount(uint32_t port, uint32_t slot);
/** Register both guest handlers. Returns the number registered (2). */
size_t mu_register(void);
#endif
