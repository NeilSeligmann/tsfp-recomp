/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "shot_hotkey.h"
#include "d3d8_overlay_image.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

bool shot_png_write(const char *path, uint32_t width, uint32_t height, const uint8_t *rgb)
{
    return d3d8_overlay_image_write_png(path, rgb, width, height);
}

uint64_t shot_png_size(uint32_t width, uint32_t height)
{
    const uint64_t raw = (uint64_t)((uint64_t)width * 3u + 1u) * height;
    const uint64_t blocks = (raw + 65534u) / 65535u;
    return 8u + (12u + 13u) + (12u + 2u + raw + 5u * blocks + 4u) + 12u; /* signature, IHDR, IDAT (zlib, stored blocks), IEND */
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

bool shot_bmp_load(const char *path, uint32_t *width, uint32_t *height, uint8_t **rgb)
{
    if (path == NULL || width == NULL || height == NULL || rgb == NULL) return false;
    *rgb = NULL;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    uint8_t head[54];
    if (fread(head, 1, sizeof head, file) != sizeof head || head[0] != 'B' || head[1] != 'M') {
        fclose(file);
        return false;
    }
    const uint32_t offset = le32(head + 10);
    const int32_t w = (int32_t)le32(head + 18);
    const int32_t h = (int32_t)le32(head + 22);
    const unsigned bits = (unsigned)head[28] | (unsigned)head[29] << 8;
    const uint32_t compression = le32(head + 30);
    const bool top_down = h < 0;
    const uint32_t rows = top_down ? (uint32_t)(-(int64_t)h) : (uint32_t)h;
    if (w <= 0 || rows == 0u || w > 16384 || rows > 16384u || (bits != 24u && bits != 32u) || (compression != 0u && compression != 3u)) {
        fclose(file);
        return false;
    }
    const size_t stride = ((size_t)w * bits / 8u + 3u) & ~(size_t)3u;
    uint8_t *line = malloc(stride);
    uint8_t *out = malloc((size_t)w * rows * 3u);
    bool good = line != NULL && out != NULL && fseek(file, (long)offset, SEEK_SET) == 0;
    for (uint32_t y = 0u; good && y < rows; y++) {
        if (fread(line, 1, stride, file) != stride) {
            good = false;
            break;
        }
        uint8_t *dest = out + (size_t)(top_down ? y : rows - 1u - y) * (size_t)w * 3u;
        for (int32_t x = 0; x < w; x++) {
            const uint8_t *px = line + (size_t)x * (bits / 8u); /* B G R [A] */
            dest[x * 3] = px[2];
            dest[x * 3 + 1] = px[1];
            dest[x * 3 + 2] = px[0];
        }
    }
    fclose(file);
    free(line);
    if (!good) {
        free(out);
        return false;
    }
    *width = (uint32_t)w;
    *height = rows;
    *rgb = out;
    return true;
}

size_t shot_manifest_format(char *out, size_t size, const char *png_name, uint64_t poll, uint64_t present, const char *phase,
                            const char *label, unsigned hotkey_number, uint32_t width, uint32_t height, uint64_t unix_seconds,
                            uint64_t png_bytes)
{
    char clean[SHOT_PHASE_MAX];
    size_t n = 0u;
    for (; phase != NULL && phase[n] != '\0' && n + 1u < sizeof clean; n++) {
        const char c = phase[n];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        clean[n] = ok ? c : '_';
    }
    if (n == 0u) clean[n++] = '-';
    clean[n] = '\0';
    const int written = snprintf(out, size, "%s poll=%llu present=%llu phase=%s label=%s hotkey=%u size=%ux%u unix=%llu bytes=%llu", png_name,
                                 (unsigned long long)poll, (unsigned long long)present, clean, label != NULL ? label : "shot",
                                 hotkey_number, width, height, (unsigned long long)unix_seconds, (unsigned long long)png_bytes);
    return written > 0 && (size_t)written < size ? (size_t)written : 0u;
}

/* mkdir -p. An existing directory is fine, anything else on the way is not. */
static bool make_directories(const char *path)
{
    char copy[SHOT_PATH_MAX];
    if (strlen(path) >= sizeof copy) return false;
    strcpy(copy, path);
    for (char *cursor = copy + 1; ; cursor++) {
        if (*cursor == '/' || *cursor == '\0') {
            const char saved = *cursor;
            *cursor = '\0';
            struct stat info;
            if (mkdir(copy, 0777) != 0 && !(errno == EEXIST && stat(copy, &info) == 0 && S_ISDIR(info.st_mode))) return false;
            *cursor = saved;
            if (saved == '\0') return true;
        }
    }
}

bool shot_hotkey_init(shot_hotkey *shot, const char *dir, bool (*capture)(void *, const char *), uint64_t (*presented)(void *),
                      void *user)
{
    if (shot == NULL) return false;
    memset(shot, 0, sizeof *shot);
    if (dir == NULL || capture == NULL || strlen(dir) + 40u >= SHOT_PATH_MAX) return false;
    snprintf(shot->dir, sizeof shot->dir, "%s", dir);
    shot->capture = capture;
    shot->presented = presented;
    shot->user = user;
    shot->max_shots = SHOT_MAX_DEFAULT;
    shot->max_bytes = SHOT_MAX_BYTES_DEFAULT;
    shot->next_number = 1u;
    /* Best effort: a directory that cannot be made is not fatal, every press then fails soft (counted failed, one message). */
    if (!make_directories(dir)) fprintf(stderr, "hotkey (T1720): shot directory '%s' cannot be created, shots will fail\n", dir);
    return true;
}

void shot_hotkey_set_bounds(shot_hotkey *shot, unsigned max_shots, uint64_t max_bytes)
{
    if (shot == NULL) return;
    if (max_shots != 0u) shot->max_shots = max_shots;
    if (max_bytes != 0u) shot->max_bytes = max_bytes;
}

void shot_hotkey_set_phase_dir(shot_hotkey *shot, const char *dir)
{
    if (shot == NULL) return;
    snprintf(shot->phase_dir, sizeof shot->phase_dir, "%s", dir != NULL ? dir : "");
}

size_t shot_hotkey_summary(const shot_hotkey *shot, char *out, size_t size)
{
    if (shot == NULL || out == NULL || size == 0u) return 0u;
    const int written = snprintf(out, size, "shots (T1720) taken %u refused %u failed %u bytes %llu dir %s", shot->taken, shot->refused,
                                 shot->failed, (unsigned long long)shot->bytes, shot->dir);
    return written > 0 && (size_t)written < size ? (size_t)written : 0u;
}

static bool read_phase_file(const char *dir, char *phase, size_t size)
{
    char path[SHOT_PATH_MAX];
    snprintf(path, sizeof path, "%s/phase", dir);
    FILE *file = fopen(path, "r");
    if (file == NULL) return false;
    const bool got = fgets(phase, (int)size, file) != NULL;
    fclose(file);
    return got;
}

static void read_phase(const shot_hotkey *shot, char *phase, size_t size)
{
    phase[0] = '\0';
    if (!(shot->phase_dir[0] != '\0' && read_phase_file(shot->phase_dir, phase, size)) && !read_phase_file(shot->dir, phase, size)) phase[0] = '\0';
    phase[strcspn(phase, "\r\n")] = '\0';
}

static bool path_exists(const char *path)
{
    struct stat info;
    return lstat(path, &info) == 0; /* lstat: a dangling symlink also holds the name */
}

/* Flush a finished file to the disk (the PNG writer closed it, so it is reopened). */
static bool sync_file(const char *path)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    const bool good = fsync(fd) == 0;
    close(fd);
    return good;
}

static void sync_directory(const char *dir)
{
    const int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return;
    (void)fsync(fd);
    close(fd);
}

/* Appends one complete line with ONE write() on an O_APPEND descriptor, so a crash or a second writer never leaves half a line
 * in the middle. A short write is cut back off the file. */
static bool manifest_append(const char *path, const char *line)
{
    char record[SHOT_PATH_MAX + 2u];
    const size_t length = (size_t)snprintf(record, sizeof record, "%s\n", line);
    const int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (fd < 0) return false;
    struct stat before;
    const bool have_size = fstat(fd, &before) == 0;
    ssize_t done;
    do {
        done = write(fd, record, length);
    } while (done < 0 && errno == EINTR);
    bool good = done == (ssize_t)length;
    if (!good && done > 0 && have_size) (void)ftruncate(fd, before.st_size);
    good = close(fd) == 0 && good;
    return good;
}

bool shot_hotkey_take(shot_hotkey *shot, unsigned hotkey_number, const char *label, uint64_t poll)
{
    if (shot == NULL || shot->capture == NULL) return false;
    /* Bounds first: a refused press captures nothing and takes no number. The size of the next picture is estimated by the last
     * one (the first by the 640x480 stored-deflate size), the exact size is checked again once the frame is read. */
    if (shot->taken >= shot->max_shots) {
        shot->refused++;
        fprintf(stderr, "hotkey (T1720): 'shot' #%u at host poll %llu REFUSED: --shot-max %u pictures reached\n", hotkey_number,
                (unsigned long long)poll, shot->max_shots);
        return false;
    }
    const uint64_t estimate = shot->last_bytes != 0u ? shot->last_bytes : shot_png_size(640u, 480u);
    if (shot->bytes + estimate > shot->max_bytes) {
        shot->refused++;
        fprintf(stderr, "hotkey (T1720): 'shot' #%u at host poll %llu REFUSED: --shot-max-bytes %llu would be exceeded (%llu used, ~%llu next)\n",
                hotkey_number, (unsigned long long)poll, (unsigned long long)shot->max_bytes, (unsigned long long)shot->bytes,
                (unsigned long long)estimate);
        return false;
    }
    char bmp[SHOT_PATH_MAX], png[SHOT_PATH_MAX], tmp[SHOT_PATH_MAX], name[32], phase[SHOT_PHASE_MAX], line[SHOT_PATH_MAX];
    /* never overwrite: skip the numbers whose file is already there */
    unsigned number = shot->next_number;
    for (;; number++) {
        snprintf(name, sizeof name, "shot-%03u.png", number);
        snprintf(png, sizeof png, "%s/%s", shot->dir, name);
        if (!path_exists(png) || number == 0xFFFFFFFFu) break;
    }
    shot->next_number = number + 1u;
    snprintf(bmp, sizeof bmp, "%s/.shot-%03u.bmp", shot->dir, number);
    snprintf(tmp, sizeof tmp, "%s/.%s.tmp", shot->dir, name);
    uint32_t width = 0u, height = 0u;
    uint8_t *rgb = NULL;
    const uint64_t present = shot->presented != NULL ? shot->presented(shot->user) : 0u;
    bool good = shot->capture(shot->user, bmp) && shot_bmp_load(bmp, &width, &height, &rgb);
    remove(bmp);
    if (good && shot->bytes + shot_png_size(width, height) > shot->max_bytes) {
        free(rgb);
        shot->refused++;
        shot->next_number = number; /* refused, so the number is not used */
        fprintf(stderr, "hotkey (T1720): 'shot' #%u at host poll %llu REFUSED: --shot-max-bytes %llu would be exceeded (%llu used, %llu for %ux%u)\n",
                hotkey_number, (unsigned long long)poll, (unsigned long long)shot->max_bytes, (unsigned long long)shot->bytes,
                (unsigned long long)shot_png_size(width, height), width, height);
        return false;
    }
    /* temp file in the same directory, flushed to disk, then renamed: a reader or a crash sees the whole PNG or none */
    struct stat info;
    good = good && shot_png_write(tmp, width, height, rgb) && sync_file(tmp) && stat(tmp, &info) == 0 && rename(tmp, png) == 0;
    free(rgb);
    if (!good) {
        remove(tmp);
        shot->failed++;
        fprintf(stderr, "hotkey (T1720): 'shot' #%u at host poll %llu FAILED (the window sink captures only with --present window, and %s must be writable)\n",
                hotkey_number, (unsigned long long)poll, shot->dir);
        return false;
    }
    sync_directory(shot->dir);
    const uint64_t png_bytes = (uint64_t)info.st_size;
    shot->bytes += png_bytes;
    shot->last_bytes = png_bytes;
    read_phase(shot, phase, sizeof phase);
    char manifest[SHOT_PATH_MAX];
    snprintf(manifest, sizeof manifest, "%s/shots.manifest", shot->dir);
    if (shot_manifest_format(line, sizeof line, name, poll, present, phase, label, hotkey_number, width, height, (uint64_t)time(NULL),
                             png_bytes) == 0u ||
        !manifest_append(manifest, line)) {
        shot->failed++;
        fprintf(stderr, "hotkey (T1720): 'shot' #%u at host poll %llu wrote %s but its shots.manifest line FAILED\n", hotkey_number,
                (unsigned long long)poll, name);
        return false;
    }
    shot->taken++;
    fprintf(stderr, "hotkey (T1720): 'shot' #%u at host poll %llu -> %s (%ux%u, %llu bytes)\n", hotkey_number, (unsigned long long)poll, name,
            width, height, (unsigned long long)png_bytes);
    return true;
}
