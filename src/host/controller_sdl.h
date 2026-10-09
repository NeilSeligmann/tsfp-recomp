/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_CONTROLLER_SDL_H
#define TSFP_CONTROLLER_SDL_H
#include "present_sink.h"
#include "controller_config.h"
/* SDL handles/event pumping belong to the existing presenter thread. The sink must
 * outlive the controller backend. This opt-in uses the modeled guest pad adapter;
 * host-device support does not establish USB/XDK hardware equivalence. */
typedef struct controller_sdl controller_sdl;
controller_sdl *controller_sdl_open(present_video_sink *sink, const controller_config *config,
                                  const char *mapping_file, char *error, size_t error_size);
void controller_sdl_install(controller_sdl *controllers);
void controller_sdl_pump(controller_sdl *controllers);
void controller_sdl_close(controller_sdl *controllers);
/* Emergency process exit: stop host motors without freeing callback state still
 * owned by unjoined guest threads. Does not change guest device tables. */
void controller_sdl_cancel(controller_sdl *controllers);
/* Live assignment follows the same validated persistent selector rules. Stops old
 * feedback and clears old state before exposing the newly selected device. */
bool controller_sdl_assign(controller_sdl *controllers, unsigned port, const controller_selector *selector);
size_t controller_sdl_describe(controller_sdl *controllers, char *buffer, size_t size);
#endif
