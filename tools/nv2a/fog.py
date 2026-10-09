# SPDX-License-Identifier: GPL-3.0-or-later
"""Pinned xemu programmable post-vertex fog, before varying interpolation."""

GLSL = r"""
float live_fog_factor(float distance, vec4 control) {
    if (control.x == 0.0) return 1.0;
    int mode = int(control.y);
    float exceptional = (mode <= 1 || mode == 3) ? 1.0 : 0.0;
    if (isinf(distance)) return exceptional;
    float value;
    if (mode == 0 || mode == 3) value = (control.z + distance * control.w) - 1.0;
    else if (mode == 1 || mode == 4) value = (control.z + exp2(distance * control.w * 16.0)) - 1.5;
    else value = (control.z + exp2(-distance * distance * control.w * control.w * 32.0)) - 1.5;
    if (mode >= 3) value = abs(value);
    if (isnan(value)) value = exceptional;
    return clamp(value, -3.4028234663852886e+38, 3.4028234663852886e+38);
}
"""
