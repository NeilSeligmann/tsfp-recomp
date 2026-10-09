/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_XNET_NVNET_H
#define TSFP_XNET_NVNET_H
#include <stdbool.h>
#include <stdint.h>
/* INFERRED opt-in device register core grounded in xemu0.8.136 fc24584c.
 * Explicit no-peer reset. No network client, packet success, DMA completion,
 * fabricated RX, physical-address translation or whole-NIC admission. */
typedef struct {
    uint8_t registers[1024];
    uint32_t phy[6];
    bool irq_asserted;
} xnet_nvnet;
bool xnet_nvnet_reset_no_peer(xnet_nvnet *device);
bool xnet_nvnet_read(const xnet_nvnet *device, uint32_t offset, unsigned bytes, uint32_t *value);
/* Executes no-peer/disabled DMA kick as the original source no-op; refuses a
 * transmit-eligible kick before writes. No TX descriptor or packet is completed. */
bool xnet_nvnet_write(xnet_nvnet *device, uint32_t offset, unsigned bytes, uint32_t value);
#endif
