/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_XNET_PHY_H
#define TSFP_XNET_PHY_H
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include "xnet_nvnet.h"
/* Source-grounded Complex4627 PHY state, no-peer NVNET register core. Genuine
 * virtual delay callback required; no forced PHY/NIC success or invented cache.
 * Caller initializes a fresh context once, then preserves it across guest calls. */
typedef struct {
    xnet_nvnet *device;
    void (*delay)(void *context, uint32_t microseconds);
    void *delay_context;
    atomic_uint busy;
    atomic_uint flags;
    atomic_uint initialized;
    uint8_t mcp_revision;
} xnet_phy;
bool xnet_phy_context_init(xnet_phy *phy, xnet_nvnet *device,
                          void (*delay)(void *, uint32_t), void *context, uint8_t mcp_revision);
/* Host false means invocation/source refusal; status/link is actual original
 * 253/252 output only when true. Argument2 is unused by original253. */
bool xnet_phy_initialize(xnet_phy *phy, uint32_t reset, uint32_t argument2, uint32_t *status);
bool xnet_phy_get_link_state(xnet_phy *phy, uint32_t refresh, uint32_t *link);
#endif
