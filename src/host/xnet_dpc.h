/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_HOST_XNET_DPC_H
#define TSFP_HOST_XNET_DPC_H
#include <stdbool.h>
/* Quiescent configuration; elapsed-time coalescing remains INFERRED and opt-in.
 * Allocates a private callback stack lazily for actual queued43A184 only.
 * Does not create a singleton, timer, NIC or successful deferred operation. */
bool host_xnet_dpc_configure(bool enabled);
void host_xnet_dpc_service_hook(void);
bool host_xnet_dpc_shutdown(void);
#endif
