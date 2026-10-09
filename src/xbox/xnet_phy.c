/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xnet_phy.h"

static bool mdio(xnet_phy *phy, unsigned reg, bool write, uint32_t *value, bool *completed)
{
    uint32_t address;
    if (!phy->device || !phy->delay || !value || !completed ||
        !xnet_nvnet_read(phy->device, 0x190u, 4u, &address) || (address & 0x8000u))
        return false; /* Original busy-recovery helper is outside this admitted core. */
    if (write && !xnet_nvnet_write(phy->device, 0x194u, 4u, *value)) return false;
    address = reg | (write ? 0x420u : 0x20u);
    if (!xnet_nvnet_write(phy->device, 0x190u, 4u, address)) return false;
    address |= 0x8000u;
    int remaining = 0x2000;
    while ((address & 0x8000u) && remaining > 0) {
        phy->delay(phy->delay_context, 50u);
        if (!xnet_nvnet_read(phy->device, 0x190u, 4u, &address)) return false;
        remaining -= 50;
    }
    if (!write && !xnet_nvnet_read(phy->device, 0x194u, 4u, value)) return false;
    *completed = !(address & 0x8000u);
    return true;
}
static bool read_phy(xnet_phy *phy, unsigned reg, uint32_t *value, bool *completed)
{
    return mdio(phy, reg, false, value, completed);
}
static bool refresh_flags(xnet_phy *phy, bool *completed)
{
    uint32_t local, peer, status;
    if (!read_phy(phy, 4u, &local, completed)) return false;
    if (!*completed) return true;
    if (!read_phy(phy, 5u, &peer, completed)) return false;
    if (!*completed) return true;
    if (!read_phy(phy, 1u, &status, completed)) return false;
    if (!*completed) return true;
    const uint32_t common = local & peer;
    uint32_t flags = common & 0x180u ? 2u : common & 0x60u ? 4u : 0u;
    if (common & 0x140u) flags |= 8u;
    else if (common & 0xa0u) flags |= 16u;
    if (status & 4u) flags |= 1u;
    phy->flags = flags;
    *completed = true;
    return true;
}
static bool poll_link(xnet_phy *phy, uint32_t *status)
{
    *status = 0u;
    for (unsigned remaining = 1000u; remaining && !(*status & 4u); --remaining) {
        bool completed;
        phy->delay(phy->delay_context, 500u);
        if (!read_phy(phy, 1u, status, &completed)) return false;
        if (!completed) break;
    }
    return true;
}
bool xnet_phy_context_init(xnet_phy *phy, xnet_nvnet *device,
                          void (*delay)(void *, uint32_t), void *context, uint8_t mcp_revision)
{
    if (!phy || !device || !delay) return false;
    phy->device = device;
    phy->delay = delay;
    phy->delay_context = context;
    atomic_init(&phy->busy, 0u);
    atomic_init(&phy->flags, 0u);
    atomic_init(&phy->initialized, 0u);
    phy->mcp_revision = mcp_revision;
    return true;
}
bool xnet_phy_initialize(xnet_phy *phy, uint32_t reset, uint32_t argument2, uint32_t *status)
{
    (void)argument2;
    if (!phy || !phy->device || !phy->delay || !status) return false;
    unsigned expected = 0u;
    if (!atomic_compare_exchange_strong(&phy->busy, &expected, 1u)) {
        *status = 0x800700aau;
        return true;
    }
    uint32_t result = 0x801f0001u, control = 0u, link = 0u;
    bool completed = false, invoked = true;
    if (reset) {
        phy->initialized = false;
        phy->flags = 0u;
        control = 0x8000u;
        if (!mdio(phy, 0u, true, &control, &completed)) { invoked = false; goto done; }
        if (!completed) goto done;
        for (unsigned remaining = 1000u; remaining && (control & 0x8000u); --remaining) {
            phy->delay(phy->delay_context, 500u);
            if (!read_phy(phy, 0u, &control, &completed)) { invoked = false; goto done; }
            if (!completed) goto done;
        }
        if (control & 0x8000u) goto done;
    } else if (phy->initialized) {
        if (!refresh_flags(phy, &completed)) { invoked = false; goto done; }
        result = 0u; /* Original warm path ignores refresh helper BOOL. */
        goto done;
    }
    for (unsigned remaining = 6000u; remaining && !(link & 0x20u); --remaining) {
        phy->delay(phy->delay_context, 500u);
        if (!read_phy(phy, 1u, &link, &completed)) { invoked = false; goto done; }
        if (!completed) goto done;
    }
    /* Revision-specific register18 write is absent on the xemu six-register PHY;
     * read returns0, so the original read/clear/write sequence still executes. */
    if (phy->mcp_revision != 0xa1u) {
        uint32_t extension;
        if (!read_phy(phy, 24u, &extension, &completed)) { invoked = false; goto done; }
        if (completed) {
            extension &= ~0x100u;
            if (!mdio(phy, 24u, true, &extension, &completed)) { invoked = false; goto done; }
        }
    }
    if (!read_phy(phy, 0u, &control, &completed)) { invoked = false; goto done; }
    if (!completed) goto done;
    if (control & 0x200u) {
        if (link & 0xa200u) {
            control = (control & ~0x40u) | 0x2000u;
            phy->flags |= 2u;
        } else if (link & 0x800u) {
            control = (control & ~0x2000u) | 0x40u;
            phy->flags |= 4u;
        } else goto done;
        phy->flags |= 16u;
        if (!mdio(phy, 0u, true, &control, &completed) || !poll_link(phy, &link)) {
            invoked = false; goto done;
        }
        if (link & 4u) phy->flags |= 1u;
    } else {
        if (!poll_link(phy, &link) || !refresh_flags(phy, &completed)) { invoked = false; goto done; }
        if (!completed) goto done;
    }
    phy->initialized = true;
    result = 0u;
done:
    atomic_store(&phy->busy, 0u);
    if (!invoked) return false;
    *status = result;
    return true;
}
bool xnet_phy_get_link_state(xnet_phy *phy, uint32_t refresh, uint32_t *link)
{
    if (!phy || !phy->device || !phy->delay || !link) return false;
    if (!phy->flags || refresh) {
        unsigned expected = 0u;
        if (atomic_compare_exchange_strong(&phy->busy, &expected, 1u)) {
            bool completed;
            const bool invoked = refresh_flags(phy, &completed);
            atomic_store(&phy->busy, 0u);
            if (!invoked) return false;
        }
    }
    *link = phy->flags;
    return true;
}
