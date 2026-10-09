/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_LIVE_VK_QUERY_H
#define TSFP_LIVE_VK_QUERY_H
#include "live_vk_pipeline.h"
typedef struct live_vk_query live_vk_query;
/* Precise counts only: unsupported devices are refused, never boolean stand-ins. */
live_vk_query *live_vk_query_create(const live_vk_device *device);
void live_vk_query_destroy(live_vk_query *query);
/* Caller must have submitted/completed every preceding use before reset. */
bool live_vk_query_reset(live_vk_query *query);
/* Record inside an active render pass. Each draw receives an independent slot. */
bool live_vk_query_begin(live_vk_query *query, VkCommandBuffer command, uint32_t *slot);
void live_vk_query_end(live_vk_query *query, VkCommandBuffer command, uint32_t slot);
/* No waiting or fabricated result. False means unavailable/device failure. */
bool live_vk_query_result(live_vk_query *query, uint32_t slot, uint64_t *samples);
#endif
