/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_XVOICE_MEDIA_H
#define TSFP_XVOICE_MEDIA_H

#include <stddef.h>
#include <stdint.h>

/* Register only the title-measured disconnected-device result. No connected
 * USB voice media object is represented by this bounded handler. */
size_t xvoice_media_register(void);

/* xinput HLE entry for retail XVoiceCreateMediaObjectEx at 0x004754FD. */
uint32_t xvoice_media_create_ex(void *context);

#endif /* TSFP_XVOICE_MEDIA_H */
