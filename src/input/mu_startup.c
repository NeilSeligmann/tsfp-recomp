/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mu_startup.h"
#include "mu_device.h"
#include "xinput_devices.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    const mu_startup_image *images;
    size_t count;
    char *error;
    size_t error_size;
    bool result;
} startup_request;
static void fail(startup_request *r, size_t index, const char *reason)
{
    if (r->error != NULL && r->error_size != 0u)
        (void)snprintf(r->error, r->error_size, "MU image %zu: %s", index, reason);
}
static void attach_job(void *opaque)
{
    startup_request *r = opaque;
    if (r->count > MU_STARTUP_MAX || (r->count != 0u && r->images == NULL)) {
        fail(r, 0u, "invalid image list"); return;
    }
    unsigned used = 0u;
    for (size_t i = 0u; i < r->count; i++) {
        const mu_startup_image *image = &r->images[i];
        if (image->port >= MU_PORTS || image->slot >= MU_SLOTS || image->path == NULL || image->path[0] == '\0') {
            fail(r, i, "expected port 0..3, slot 0..1 and a nonempty image path"); return;
        }
        const unsigned bit = 1u << (image->port * MU_SLOTS + image->slot);
        if ((used & bit) != 0u || mu_attached(image->port, image->slot)) {
            fail(r, i, "duplicate or already attached slot"); return;
        }
        used |= bit;
    }
    size_t attached = 0u;
    for (; attached < r->count; attached++) {
        const mu_startup_image *image = &r->images[attached];
        /* Validate write access without writing/creating/truncating the image. */
        FILE *file = fopen(image->path, "r+b");
        if (file == NULL) { fail(r, attached, "cannot open existing image for read/write"); break; }
        if (fclose(file) != 0) { fail(r, attached, "cannot close image access check"); break; }
        if (!mu_attach_file(image->port, image->slot, image->path, 0u)) {
            fail(r, attached, "invalid image geometry or allocation/read failure"); break;
        }
    }
    if (attached == r->count) { r->result = true; return; }
    while (attached != 0u) {
        const mu_startup_image *image = &r->images[--attached];
        (void)mu_detach(image->port, image->slot);
    }
}
bool mu_startup_attach(const mu_startup_image *images, size_t count, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0u) error[0] = '\0';
    startup_request r = {.images = images, .count = count, .error = error, .error_size = error_size};
    xinput_devices_run_locked(attach_job, &r);
    return r.result;
}
