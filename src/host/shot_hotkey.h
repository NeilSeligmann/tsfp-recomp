/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_HOST_SHOT_HOTKEY_H
#define TSFP_HOST_SHOT_HOTKEY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* T1720: the host side of the `shot` hotkey label. A press captures the presented frame (the same window read-back as
 * --present-capture, a BMP), converts it to DIR/shot-NNN.png with the in-tree PNG writer below (no zlib: stored deflate
 * blocks, so the files are large but valid) and appends one line to DIR/shots.manifest:
 *
 *   shot-001.png poll=912 present=340 phase=ingame-idle label=shot hotkey=3 size=640x480 unix=1790000000 bytes=921654
 *
 * unix = time(NULL) at the capture, bytes = the PNG file size (both added by T1720b, a tool reading old manifests treats them as
 * optional). The PNG is written to DIR/.shot-NNN.png.tmp, flushed, then rename()d, and the manifest line is one write() on an
 * O_APPEND descriptor, so a crash leaves a whole file or none and never half a line. A number whose file exists is skipped.
 * A press beyond --shot-max pictures or --shot-max-bytes is REFUSED (no capture, no number used).
 *
 * poll = host pad poll the chord fired on (the same number as DIR/hotkey.<n>), present = pictures the window had presented,
 * phase = first line of DIR/phase (the hotkey dir, else the shot dir) when that file exists (a wrapper script writes the phase name there), else "-". Tooling only:
 * nothing here is a game input and the guest is not touched. The capture runs on the guest thread inside the pad poll, outside
 * the sink lock, exactly where the stop-time --present-capture runs. */
#define SHOT_PATH_MAX 512u
#define SHOT_PHASE_MAX 64u

#define SHOT_MAX_DEFAULT 200u               /* same as HOST_OPTIONS_SHOT_MAX_DEFAULT (--shot-max), a test pins it */
#define SHOT_MAX_BYTES_DEFAULT 536870912ull /* same as HOST_OPTIONS_SHOT_MAX_BYTES_DEFAULT (--shot-max-bytes) */

/* PNG writer, a thin wrapper over d3d8_overlay_image_write_png (8 bit RGB, stored deflate, no zlib): `rgb` is width*height*3 bytes,
 * top row first. False on bad arguments or a write error. Writes `path` directly, the caller does the temp file and the rename. */
bool shot_png_write(const char *path, uint32_t width, uint32_t height, const uint8_t *rgb);
/* Exact size in bytes of the PNG shot_png_write produces for a picture of that size. */
uint64_t shot_png_size(uint32_t width, uint32_t height);
/* Reads an uncompressed 24 or 32 bit BMP (what SDL_SaveBMP and the live capture write). *rgb is malloc'ed, top row first. */
bool shot_bmp_load(const char *path, uint32_t *width, uint32_t *height, uint8_t **rgb);
/* One manifest line, without the newline. Returns the length, 0 when `size` is too small. A phase with anything but
 * [A-Za-z0-9_.-] characters is cleaned (other characters become '_'), an empty phase becomes "-". */
size_t shot_manifest_format(char *out, size_t size, const char *png_name, uint64_t poll, uint64_t present, const char *phase,
                            const char *label, unsigned hotkey_number, uint32_t width, uint32_t height, uint64_t unix_seconds,
                            uint64_t png_bytes);

typedef struct {
    char dir[SHOT_PATH_MAX];
    bool (*capture)(void *user, const char *bmp_path); /* write the presented frame as a BMP */
    uint64_t (*presented)(void *user);                 /* pictures presented so far */
    void *user;
    unsigned max_shots;     /* --shot-max: pictures per run */
    uint64_t max_bytes;     /* --shot-max-bytes: PNG bytes per run */
    unsigned next_number;   /* lowest number not tried yet */
    unsigned taken, failed; /* pictures written; presses that tried and failed (they use up their number) */
    unsigned refused;       /* presses turned away by a bound (no capture, no number) */
    uint64_t bytes;         /* PNG bytes written so far */
    uint64_t last_bytes;    /* size of the last PNG, the estimate for the next press */
    char phase_dir[SHOT_PATH_MAX]; /* where a wrapper writes the `phase` file first (empty = only the shot dir) */
} shot_hotkey;

/* `dir` is created when missing (mkdir -p, failing to is reported once and every press then fails soft). Returns false when it
 * is too long or missing a mandatory hook. The bounds start at SHOT_MAX_DEFAULT and SHOT_MAX_BYTES_DEFAULT. */
bool shot_hotkey_init(shot_hotkey *shot, const char *dir, bool (*capture)(void *, const char *), uint64_t (*presented)(void *),
                      void *user);
/* 0 keeps the current value of that bound. */
void shot_hotkey_set_bounds(shot_hotkey *shot, unsigned max_shots, uint64_t max_bytes);
/* T1720b: with --shot-dir apart from --hotkey-dir the wrapper script keeps writing `phase` into the hotkey dir: it is read there
 * first, then in the shot dir. */
void shot_hotkey_set_phase_dir(shot_hotkey *shot, const char *dir);
/* Take the next free shot number. True when the PNG and the manifest line were written, false when refused or failed. */
bool shot_hotkey_take(shot_hotkey *shot, unsigned hotkey_number, const char *label, uint64_t poll);
/* "shots (T1720) taken N refused M failed K bytes B dir D", the end-of-run line. Returns the length, 0 when `size` is too small. */
size_t shot_hotkey_summary(const shot_hotkey *shot, char *out, size_t size);

#endif
