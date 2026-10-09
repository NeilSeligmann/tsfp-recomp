/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xnet_nvnet.h"
#include <string.h>

static bool access_ok(uint32_t offset, unsigned bytes)
{
    return (bytes == 1u || bytes == 2u || bytes == 4u) &&
           offset <= 1024u - bytes && !(offset & (bytes - 1u));
}
static uint32_t get(const xnet_nvnet *device, uint32_t offset, unsigned bytes)
{
    uint32_t result = 0u;
    for (unsigned i = 0u; i < bytes; ++i) result |= (uint32_t)device->registers[offset + i] << (8u * i);
    return result;
}
static void set(xnet_nvnet *device, uint32_t offset, unsigned bytes, uint32_t value)
{
    for (unsigned i = 0u; i < bytes; ++i) device->registers[offset + i] = (uint8_t)(value >> (8u * i));
}
static void irq(xnet_nvnet *device)
{
    device->irq_asserted = (get(device, 0u, 4u) & get(device, 4u, 4u)) != 0u;
}
bool xnet_nvnet_reset_no_peer(xnet_nvnet *device)
{
    if (!device) return false;
    memset(device, 0, sizeof(*device));
    device->phy[0] = 0x1100u; /* FD + AUTOEN. */
    device->phy[1] = 8u; /* AUTONEG, no LINK_ST/AN_COMP with peer link down. */
    device->phy[4] = 0x3e0u; device->phy[5] = 0x3e0u;
    set(device, 0x144u, 4u, 8u); /* DMA idle. */
    return true;
}
bool xnet_nvnet_read(const xnet_nvnet *device, uint32_t offset, unsigned bytes, uint32_t *value)
{
    if (!device || !value || !access_ok(offset, bytes)) return false;
    *value = get(device, offset, bytes);
    return true;
}
bool xnet_nvnet_write(xnet_nvnet *device, uint32_t offset, unsigned bytes, uint32_t value)
{
    if (!device || !access_ok(offset, bytes) || (offset & 3u) ||
        (offset == 0x190u && bytes != 4u) ||
        (offset == 0x144u && (value & 1u) && (get(device, 0x84u, 4u) & 1u) &&
         !(value & 4u) && (device->phy[1] & 4u))) return false;
    if (offset == 0x190u) {
        set(device, offset, 4u, value);
        const unsigned address = (value & 0x3e0u) >> 5;
        const unsigned reg = value & 31u;
        if (value & 0x400u) {
            if (address == 1u && reg < 6u) device->phy[reg] = (uint16_t)get(device, 0x194u, 4u);
        } else set(device, 0x194u, 4u, address == 1u ? (reg < 6u ? (uint16_t)device->phy[reg] : 0u) : UINT32_MAX);
        set(device, offset, 4u, value & ~0x8000u);
    } else if (offset == 0x144u) {
        /* Same xemu mask expression: preserve idle unless explicitly retained. */
        const uint32_t mask = ~8u;
        set(device, offset, 4u, (get(device, offset, 4u) & (value | ~mask)) | (value & mask));
        if (value & 16u) {
            const uint32_t tx = get(device, 0x100u, 4u), rx = get(device, 0x104u, 4u);
            set(device, 0x11cu, 4u, tx); set(device, 0x134u, 4u, tx);
            set(device, 0x120u, 4u, rx); set(device, 0x138u, 4u, rx);
        }
        if (value & 2u) set(device, 0u, 4u, 0u);
        else if (!value) set(device, 0x130u, 4u, 0x80000000u);
    } else if (offset == 0u || offset == 0x180u) {
        set(device, offset, bytes, get(device, offset, bytes) & ~value);
        irq(device);
    } else {
        set(device, offset, bytes, value);
        if (offset == 4u) irq(device);
    }
    return true;
}
