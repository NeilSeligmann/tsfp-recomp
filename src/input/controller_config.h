/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_CONTROLLER_CONFIG_H
#define TSFP_CONTROLLER_CONFIG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xinput_hle.h"

#define CONTROLLER_PORTS 4u
#define CONTROLLER_BUTTONS 15u
#define CONTROLLER_AXES 6u
#define CONTROLLER_MAPPING_PATH 1024u
/* Standard logical button order: A B X Y LB RB START BACK LSTICK RSTICK
 * DPAD_UP DOWN LEFT RIGHT GUIDE. Axes LX LY RX RY LT RT. GUIDE has no Xbox target. */
typedef struct {
    int32_t min, center, max;
    unsigned deadzone; /* normalized 0..32766, rescaled outside the deadzone */
    bool invert;
} controller_axis_calibration;
typedef struct {
    int button_map[CONTROLLER_BUTTONS]; /* target -> source, -1 disables */
    int axis_map[CONTROLLER_AXES]; /* target -> source; stick/trigger classes stay separate */
    controller_axis_calibration calibration[CONTROLLER_AXES]; /* indexed by source */
} controller_profile;
typedef struct {
    bool enabled;
    char guid[33]; /* empty=auto; otherwise 32 lowercase hexadecimal digits */
    int occurrence; /* -1=automatic; otherwise 0..7 among devices with this GUID */
} controller_selector;
typedef struct {
    unsigned version;
    char mapping_file[CONTROLLER_MAPPING_PATH]; /* optional; relative to config directory */
    controller_profile profiles[CONTROLLER_PORTS];
    controller_selector selectors[CONTROLLER_PORTS];
} controller_config;

void controller_config_defaults(controller_config *config);
bool controller_config_validate(const controller_config *config, char *error, size_t size);
/* Failure leaves *out unchanged. Parse does not load SDL mappings or access devices. */
bool controller_config_parse(const char *text, size_t len, controller_config *out, char *error, size_t size);
bool controller_config_load(const char *path, controller_config *out, char *error, size_t size);
/* Same-directory temporary file, fsync then rename; malformed config never replaces destination. */
bool controller_config_save(const char *path, const controller_config *config, char *error, size_t size);
/* Validated profiles only. SDL raw Y is negative up; defaults invert Y for Xbox positive up.
 * This applies user calibration/remaps, not a measured physical controller fidelity claim. */
void controller_profile_apply(const controller_profile *profile, const bool buttons[CONTROLLER_BUTTONS],
                              const int16_t axes[CONTROLLER_AXES], xinput_pad_state *out);
#endif
