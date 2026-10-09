/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T1720: the `shot` hotkey path without SDL: PNG writer round-trip (a tiny stored-deflate reader checks CRC, Adler and pixels),
 * BMP loader, manifest line, the full take through a fake capture and the label dispatch of hotkey_actions. */
#define _XOPEN_SOURCE 700
#include <dirent.h>
#include <ftw.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "host_options.h"
#include "hotkey_actions.h"
#include "shot_hotkey.h"

static int failures, checks;
#define CHECK(cond)                                                \
    do {                                                           \
        checks++;                                                  \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t crc32_ref(const uint8_t *d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}
static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(*size + 1u);
    if (fread(data, 1, *size, f) != *size) *size = 0;
    fclose(f);
    return data;
}

/* decode our own PNG: returns malloc'ed rgb or NULL, checks every CRC and the Adler sum */
static uint8_t *png_decode(const char *path, uint32_t *w, uint32_t *h)
{
    size_t size;
    uint8_t *file = slurp(path, &size);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (file == NULL || size < 8u || memcmp(file, sig, 8) != 0) return NULL;
    uint8_t *idat = malloc(size);
    size_t idat_size = 0u;
    bool saw_end = false;
    for (size_t at = 8u; at + 12u <= size;) {
        const uint32_t len = be32(file + at);
        if (at + 12u + len > size) return NULL;
        if (be32(file + at + 8u + len) != crc32_ref(file + at + 4u, len + 4u)) return NULL;
        if (memcmp(file + at + 4u, "IHDR", 4) == 0) {
            *w = be32(file + at + 8u);
            *h = be32(file + at + 12u);
            if (file[at + 16u] != 8u || file[at + 17u] != 2u) return NULL;
        } else if (memcmp(file + at + 4u, "IDAT", 4) == 0) {
            memcpy(idat + idat_size, file + at + 8u, len);
            idat_size += len;
        } else if (memcmp(file + at + 4u, "IEND", 4) == 0) {
            saw_end = true;
        }
        at += 12u + len;
    }
    if (!saw_end || idat_size < 6u || idat[0] != 0x78u) return NULL;
    uint8_t *raw = malloc(idat_size);
    size_t raw_size = 0u, at = 2u;
    for (bool last = false; !last;) {
        last = (idat[at] & 1u) != 0u;
        if ((idat[at] & 6u) != 0u) return NULL; /* stored only */
        const size_t n = (size_t)idat[at + 1] | (size_t)idat[at + 2] << 8;
        if ((n ^ 0xFFFFu) != ((size_t)idat[at + 3] | (size_t)idat[at + 4] << 8)) return NULL;
        memcpy(raw + raw_size, idat + at + 5u, n);
        raw_size += n;
        at += 5u + n;
    }
    uint32_t a = 1u, b = 0u;
    for (size_t i = 0; i < raw_size; i++) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    if (be32(idat + at) != ((b << 16) | a) || raw_size != (size_t)(*w * 3u + 1u) * *h) return NULL;
    uint8_t *rgb = malloc((size_t)*w * *h * 3u);
    for (uint32_t y = 0; y < *h; y++) {
        if (raw[y * (*w * 3u + 1u)] != 0u) return NULL;
        memcpy(rgb + (size_t)y * *w * 3u, raw + y * (*w * 3u + 1u) + 1u, (size_t)*w * 3u);
    }
    free(file);
    free(idat);
    free(raw);
    return rgb;
}

static bool write_bmp(const char *path, uint32_t w, uint32_t h, unsigned bits, const uint8_t *rgb)
{
    const size_t stride = ((size_t)w * bits / 8u + 3u) & ~(size_t)3u;
    uint8_t head[54] = {'B', 'M'};
    const uint32_t total = (uint32_t)(54u + stride * h);
    memcpy(head + 2, &total, 4);
    head[10] = 54;
    head[14] = 40;
    memcpy(head + 18, &w, 4);
    memcpy(head + 22, &h, 4);
    head[26] = 1;
    head[28] = (uint8_t)bits;
    FILE *f = fopen(path, "wb");
    if (f == NULL) return false; /* like a window capture into a directory that cannot be written */
    fwrite(head, 1, 54, f);
    uint8_t *line = calloc(1, stride);
    for (uint32_t y = 0; y < h; y++) { /* bottom-up */
        const uint8_t *src = rgb + (size_t)(h - 1u - y) * w * 3u;
        for (uint32_t x = 0; x < w; x++) {
            line[x * (bits / 8u)] = src[x * 3 + 2];
            line[x * (bits / 8u) + 1] = src[x * 3 + 1];
            line[x * (bits / 8u) + 2] = src[x * 3];
        }
        fwrite(line, 1, stride, f);
    }
    free(line);
    return fclose(f) == 0;
}

static uint8_t *test_picture(uint32_t w, uint32_t h)
{
    uint8_t *rgb = malloc((size_t)w * h * 3u);
    for (size_t i = 0; i < (size_t)w * h * 3u; i++) rgb[i] = (uint8_t)(i * 7u + i / 13u);
    return rgb;
}

static void test_png_roundtrip(const char *dir)
{
    char path[300];
    snprintf(path, sizeof path, "%s/rt.png", dir);
    const uint32_t sizes[][2] = {{1, 1}, {5, 3}, {3, 7}, {640, 480}, {400, 100}}; /* 640x480 spans several 64 KiB stored blocks */
    for (unsigned i = 0; i < 5u; i++) {
        uint8_t *rgb = test_picture(sizes[i][0], sizes[i][1]);
        CHECK(shot_png_write(path, sizes[i][0], sizes[i][1], rgb));
        uint32_t w = 0, h = 0;
        uint8_t *back = png_decode(path, &w, &h);
        CHECK(back != NULL && w == sizes[i][0] && h == sizes[i][1]);
        CHECK(back != NULL && memcmp(back, rgb, (size_t)w * h * 3u) == 0);
        free(back);
        free(rgb);
    }
    CHECK(!shot_png_write(path, 0, 4, (const uint8_t *)"x"));
    CHECK(!shot_png_write(path, 4, 4, NULL));
    CHECK(!shot_png_write(NULL, 1, 1, (const uint8_t *)"xxx"));
}

static void test_bmp_loader(const char *dir)
{
    char path[300];
    snprintf(path, sizeof path, "%s/in.bmp", dir);
    uint8_t *rgb = test_picture(7, 5); /* 7*3 = 21 bytes a row, padded to 24 */
    for (unsigned bits = 24u; bits <= 32u; bits += 8u) {
        write_bmp(path, 7, 5, bits, rgb);
        uint32_t w = 0, h = 0;
        uint8_t *loaded = NULL;
        CHECK(shot_bmp_load(path, &w, &h, &loaded) && w == 7u && h == 5u);
        CHECK(loaded != NULL && memcmp(loaded, rgb, 7u * 5u * 3u) == 0);
        free(loaded);
    }
    free(rgb);
    FILE *f = fopen(path, "wb");
    fputs("not a bmp at all, not a bmp at all, not a bmp at all, not a bmp at all", f);
    fclose(f);
    uint32_t w, h;
    uint8_t *loaded = NULL;
    CHECK(!shot_bmp_load(path, &w, &h, &loaded) && loaded == NULL);
    CHECK(!shot_bmp_load("/nonexistent/x.bmp", &w, &h, &loaded));
}

static void test_manifest_format(void)
{
    char line[220];
    const size_t n = shot_manifest_format(line, sizeof line, "shot-001.png", 912u, 340u, "ingame-idle", "shot", 3u, 640u, 480u, 1790000000u, 921654u);
    CHECK(n != 0u && strcmp(line, "shot-001.png poll=912 present=340 phase=ingame-idle label=shot hotkey=3 size=640x480 unix=1790000000 bytes=921654") == 0);
    shot_manifest_format(line, sizeof line, "shot-002.png", 1u, 2u, "a b\tc", "shot", 4u, 1u, 1u, 5u, 6u);
    CHECK(strstr(line, "phase=a_b_c ") != NULL);
    shot_manifest_format(line, sizeof line, "shot-003.png", 1u, 2u, "", "shot", 4u, 1u, 1u, 5u, 6u);
    CHECK(strstr(line, "phase=- ") != NULL);
    shot_manifest_format(line, sizeof line, "shot-003.png", 1u, 2u, NULL, NULL, 4u, 1u, 1u, 5u, 6u);
    CHECK(strstr(line, "phase=- label=shot ") != NULL && strstr(line, " size=1x1 unix=5 bytes=6") != NULL);
    CHECK(shot_manifest_format(line, 10u, "shot-001.png", 1u, 1u, "x", "shot", 1u, 1u, 1u, 1u, 1u) == 0u);
    /* a buffer that holds everything but the bytes field is too small too */
    CHECK(shot_manifest_format(line, 100u, "shot-001.png", 912u, 340u, "ingame-idle", "shot", 3u, 640u, 480u, 1790000000u, 921654u) == 0u);
}

/* A fake presenter frame: the capture hook writes a synthetic BMP of w*h. */
typedef struct {
    unsigned calls;        /* capture calls so far */
    unsigned fail_call;    /* this call number fails (0 = none), like a window that cannot read back */
    bool always_fail;      /* every call fails: the presented sink is not a window */
    bool garbage;          /* the capture "succeeds" but writes something that is no BMP */
    uint32_t width, height;
    const char *race_dir;  /* when set, the presented hook creates a non-empty directory shot-<race_number>.png there: the rename fails */
    unsigned race_number;
} fake_frame;
static fake_frame frame_of(unsigned fail_call, bool always_fail, bool garbage, uint32_t width, uint32_t height)
{
    fake_frame frame;
    memset(&frame, 0, sizeof frame);
    frame.fail_call = fail_call;
    frame.always_fail = always_fail;
    frame.garbage = garbage;
    frame.width = width;
    frame.height = height;
    return frame;
}
static bool fake_capture(void *user, const char *bmp_path)
{
    fake_frame *frame = user;
    frame->calls++;
    if (frame->always_fail || frame->calls == frame->fail_call) return false;
    if (frame->garbage) {
        FILE *f = fopen(bmp_path, "wb");
        if (f == NULL) return false;
        fputs("this is not a bitmap, this is not a bitmap, this is not a bitmap!!", f);
        fclose(f);
        return true;
    }
    uint8_t *rgb = test_picture(frame->width, frame->height);
    const bool written = write_bmp(bmp_path, frame->width, frame->height, 24, rgb);
    free(rgb);
    return written;
}
static void write_text(const char *dir, const char *name, const char *text);
static uint64_t fake_presented(void *user)
{
    const fake_frame *frame = user;
    if (frame->race_dir != NULL) { /* runs after the number was picked and before the PNG is renamed into place */
        char name[40], path[400];
        snprintf(name, sizeof name, "shot-%03u.png", frame->race_number);
        snprintf(path, sizeof path, "%s/%s", frame->race_dir, name);
        mkdir(path, 0777);
        write_text(path, "inner", "x");
    }
    return 100u + frame->calls;
}

/* ---- helpers for the scratch directories ---- */
static void make_dir(char *out, size_t size, const char *base, const char *name)
{
    snprintf(out, size, "%s/%s", base, name);
    mkdir(out, 0777);
}
static bool exists_in(const char *dir, const char *name)
{
    char path[400];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    struct stat info;
    return lstat(path, &info) == 0;
}
static unsigned count_matching(const char *dir, const char *prefix, const char *suffix)
{
    unsigned count = 0u;
    DIR *d = opendir(dir);
    if (d == NULL) return 0u;
    for (struct dirent *entry; (entry = readdir(d)) != NULL;) {
        const size_t name = strlen(entry->d_name), tail = strlen(suffix);
        if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0 && name >= tail && strcmp(entry->d_name + name - tail, suffix) == 0) count++;
    }
    closedir(d);
    return count;
}
static void write_text(const char *dir, const char *name, const char *text)
{
    char path[400];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    }
}
static char *read_text(const char *dir, const char *name)
{
    char path[400];
    size_t size = 0;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    char *text = (char *)slurp(path, &size);
    if (text != NULL) text[size] = '\0';
    return text;
}
static unsigned count_lines(const char *text)
{
    unsigned lines = 0u;
    for (const char *c = text; text != NULL && *c != '\0'; c++) lines += *c == '\n';
    return lines;
}
/* Redirects fd 2 to a file for the duration of `begin`/`end`, so the stderr lines of a refusal can be read back. */
static int g_saved_stderr = -1;
static void stderr_begin(const char *dir)
{
    char path[400];
    snprintf(path, sizeof path, "%s/stderr.txt", dir);
    fflush(stderr);
    g_saved_stderr = dup(2);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(fd, 2);
    close(fd);
}
static char *stderr_end(const char *dir)
{
    fflush(stderr);
    dup2(g_saved_stderr, 2);
    close(g_saved_stderr);
    return read_text(dir, "stderr.txt");
}

static void test_take(const char *dir)
{
    shot_hotkey shot;
    fake_frame frame = frame_of(3u, false, false, 16u, 9u);
    CHECK(!shot_hotkey_init(&shot, NULL, fake_capture, fake_presented, &frame));
    CHECK(!shot_hotkey_init(&shot, dir, NULL, fake_presented, &frame));
    CHECK(shot_hotkey_init(&shot, dir, fake_capture, fake_presented, &frame));
    char path[300];
    snprintf(path, sizeof path, "%s/phase", dir);
    FILE *f = fopen(path, "w");
    fputs("radar\nsecond line\n", f);
    fclose(f);
    const time_t before = time(NULL);
    CHECK(shot_hotkey_take(&shot, 7u, "shot", 912u));
    CHECK(shot_hotkey_take(&shot, 8u, "shot", 1000u));
    CHECK(!shot_hotkey_take(&shot, 9u, "shot", 1100u)); /* the third capture fails */
    CHECK(shot_hotkey_take(&shot, 10u, "shot", 1200u));
    const time_t after = time(NULL);
    CHECK(shot.taken == 3u && shot.failed == 1u && shot.refused == 0u);
    uint32_t w = 0, h = 0;
    snprintf(path, sizeof path, "%s/shot-001.png", dir);
    uint8_t *back = png_decode(path, &w, &h);
    uint8_t *want = test_picture(16, 9);
    CHECK(back != NULL && w == 16u && h == 9u && memcmp(back, want, 16u * 9u * 3u) == 0);
    free(back);
    free(want);
    snprintf(path, sizeof path, "%s/shot-003.png", dir);
    CHECK(access(path, F_OK) != 0); /* the failed one wrote nothing */
    snprintf(path, sizeof path, "%s/shot-004.png", dir);
    CHECK(access(path, F_OK) == 0);
    snprintf(path, sizeof path, "%s/.shot-001.bmp", dir);
    CHECK(access(path, F_OK) != 0); /* temp BMP removed */
    char *manifest = read_text(dir, "shots.manifest");
    CHECK(manifest != NULL);
    CHECK(strstr(manifest, "shot-001.png poll=912 present=100 phase=radar label=shot hotkey=7 size=16x9 unix=") != NULL);
    CHECK(strstr(manifest, "shot-002.png poll=1000 present=101 phase=radar label=shot hotkey=8 size=16x9 unix=") != NULL);
    CHECK(strstr(manifest, "shot-003") == NULL && strstr(manifest, "shot-004.png poll=1200 present=103") != NULL);
    /* T1720b: every line ends in unix=<time of the press> bytes=<the PNG file size>, parsed back from the file */
    char *cursor = manifest;
    unsigned parsed = 0u;
    for (char *eol; manifest != NULL && (eol = strchr(cursor, '\n')) != NULL; cursor = eol + 1) {
        *eol = '\0';
        char name[40], phase[40], label[20];
        unsigned long long poll, present, unix_seconds, bytes;
        unsigned hotkey, mw, mh;
        const int fields = sscanf(cursor, "%39s poll=%llu present=%llu phase=%39s label=%19s hotkey=%u size=%ux%u unix=%llu bytes=%llu", name, &poll,
                                  &present, phase, label, &hotkey, &mw, &mh, &unix_seconds, &bytes);
        CHECK(fields == 10);
        struct stat info;
        snprintf(path, sizeof path, "%s/%s", dir, name);
        CHECK(fields == 10 && stat(path, &info) == 0 && (unsigned long long)info.st_size == bytes && bytes == shot_png_size(16u, 9u));
        CHECK(fields == 10 && unix_seconds >= (unsigned long long)before && unix_seconds <= (unsigned long long)after);
        parsed++;
    }
    CHECK(parsed == 3u);
    free(manifest);
    CHECK(shot.bytes == 3u * shot_png_size(16u, 9u));
    CHECK(count_matching(dir, ".shot-", "") == 0u); /* no temp BMP or PNG left, after successes and the failure */
}

/* T1720b: the same bound, the same limits as the host options */
static void test_defaults_and_summary(const char *base)
{
    CHECK(SHOT_MAX_DEFAULT == HOST_OPTIONS_SHOT_MAX_DEFAULT && SHOT_MAX_BYTES_DEFAULT == HOST_OPTIONS_SHOT_MAX_BYTES_DEFAULT);
    char dir[300];
    make_dir(dir, sizeof dir, base, "summary");
    fake_frame frame = frame_of(0u, false, false, 8u, 4u);
    shot_hotkey shot;
    CHECK(shot_hotkey_init(&shot, dir, fake_capture, fake_presented, &frame));
    CHECK(shot.max_shots == 200u && shot.max_bytes == 536870912ull);
    shot_hotkey_set_bounds(&shot, 5u, 0u); /* 0 keeps the value */
    CHECK(shot.max_shots == 5u && shot.max_bytes == 536870912ull);
    shot_hotkey_set_bounds(&shot, 0u, 99999999u);
    CHECK(shot.max_shots == 5u && shot.max_bytes == 99999999ull);
    CHECK(shot_hotkey_take(&shot, 1u, "shot", 10u));
    char summary[700];
    CHECK(shot_hotkey_summary(&shot, summary, sizeof summary) != 0u);
    char want[700];
    snprintf(want, sizeof want, "shots (T1720) taken 1 refused 0 failed 0 bytes %llu dir %s", (unsigned long long)shot_png_size(8u, 4u), dir);
    CHECK(strcmp(summary, want) == 0);
    CHECK(shot_hotkey_summary(&shot, summary, 20u) == 0u);
}

static void test_png_size_matches(const char *dir)
{
    char path[300];
    snprintf(path, sizeof path, "%s/size.png", dir);
    const uint32_t sizes[][2] = {{1, 1}, {5, 3}, {16, 9}, {640, 480}, {400, 100}, {257, 255}};
    for (unsigned i = 0; i < 6u; i++) {
        uint8_t *rgb = test_picture(sizes[i][0], sizes[i][1]);
        struct stat info;
        CHECK(shot_png_write(path, sizes[i][0], sizes[i][1], rgb) && stat(path, &info) == 0 && (uint64_t)info.st_size == shot_png_size(sizes[i][0], sizes[i][1]));
        free(rgb);
    }
}

static void test_bound_by_count(const char *base)
{
    char dir[300];
    make_dir(dir, sizeof dir, base, "count");
    fake_frame frame = frame_of(0u, false, false, 16u, 9u);
    shot_hotkey shot;
    CHECK(shot_hotkey_init(&shot, dir, fake_capture, fake_presented, &frame));
    shot_hotkey_set_bounds(&shot, 3u, 0u);
    stderr_begin(dir);
    unsigned good = 0u;
    for (unsigned press = 0u; press < 5u; press++) good += shot_hotkey_take(&shot, press + 1u, "shot", 100u + press);
    char *err = stderr_end(dir);
    CHECK(good == 3u && shot.taken == 3u && shot.refused == 2u && shot.failed == 0u);
    CHECK(frame.calls == 3u); /* a refused press captures nothing */
    CHECK(exists_in(dir, "shot-001.png") && exists_in(dir, "shot-002.png") && exists_in(dir, "shot-003.png"));
    CHECK(!exists_in(dir, "shot-004.png") && !exists_in(dir, "shot-005.png"));
    CHECK(count_matching(dir, "shot-", ".png") == 3u);
    char *manifest = read_text(dir, "shots.manifest");
    CHECK(manifest != NULL && count_lines(manifest) == 3u);
    free(manifest);
    /* one stderr line per refused press, naming the bound */
    CHECK(err != NULL && strstr(err, "REFUSED: --shot-max 3 pictures reached") != NULL);
    unsigned refused_lines = 0u;
    for (const char *at = err; err != NULL && (at = strstr(at, "REFUSED")) != NULL; at++) refused_lines++;
    CHECK(refused_lines == 2u && strstr(err, "--shot-max-bytes") == NULL);
    free(err);
    /* a refused press does not consume a number: after the limit is raised the next picture is shot-004 */
    shot_hotkey_set_bounds(&shot, 4u, 0u);
    CHECK(shot_hotkey_take(&shot, 6u, "shot", 200u));
    CHECK(exists_in(dir, "shot-004.png") && !exists_in(dir, "shot-005.png") && shot.taken == 4u && shot.refused == 2u);
    /* --shot-max 1: the first press is taken, the second refused */
    char dir1[300];
    make_dir(dir1, sizeof dir1, base, "count1");
    shot_hotkey one;
    fake_frame frame1 = frame_of(0u, false, false, 4u, 4u);
    CHECK(shot_hotkey_init(&one, dir1, fake_capture, fake_presented, &frame1));
    shot_hotkey_set_bounds(&one, 1u, 0u);
    CHECK(shot_hotkey_take(&one, 1u, "shot", 1u) && !shot_hotkey_take(&one, 2u, "shot", 2u) && one.taken == 1u && one.refused == 1u);
}

static void test_bound_by_bytes(const char *base)
{
    char dir[300];
    make_dir(dir, sizeof dir, base, "bytes");
    /* 400x400 frames (~480 KB each): room for exactly two, the estimate of the third (the size of the last) does not fit */
    const uint64_t one = shot_png_size(400u, 400u);
    fake_frame frame = frame_of(0u, false, false, 400u, 400u);
    shot_hotkey shot;
    CHECK(shot_hotkey_init(&shot, dir, fake_capture, fake_presented, &frame));
    shot_hotkey_set_bounds(&shot, 0u, 2u * one + 1000u);
    stderr_begin(dir);
    CHECK(shot_hotkey_take(&shot, 1u, "shot", 1u));
    CHECK(shot_hotkey_take(&shot, 2u, "shot", 2u));
    CHECK(!shot_hotkey_take(&shot, 3u, "shot", 3u));
    CHECK(!shot_hotkey_take(&shot, 4u, "shot", 4u));
    char *err = stderr_end(dir);
    CHECK(shot.taken == 2u && shot.refused == 2u && shot.failed == 0u && shot.bytes == 2u * one);
    CHECK(frame.calls == 2u); /* the refusal came before the capture */
    CHECK(!exists_in(dir, "shot-003.png") && count_matching(dir, "shot-", ".png") == 2u);
    CHECK(err != NULL && strstr(err, "REFUSED: --shot-max-bytes") != NULL && strstr(err, "--shot-max 2") == NULL);
    free(err);
    /* the total on disk is what the counter says */
    struct stat a, b;
    char p1[400], p2[400];
    snprintf(p1, sizeof p1, "%s/shot-001.png", dir);
    snprintf(p2, sizeof p2, "%s/shot-002.png", dir);
    CHECK(stat(p1, &a) == 0 && stat(p2, &b) == 0 && (uint64_t)(a.st_size + b.st_size) == shot.bytes);
    /* a bound too small for even the first (640x480 estimate) frame refuses without a capture */
    char dir2[300];
    make_dir(dir2, sizeof dir2, base, "bytes_tiny");
    fake_frame tiny_frame = frame_of(0u, false, false, 4u, 4u);
    shot_hotkey tiny;
    CHECK(shot_hotkey_init(&tiny, dir2, fake_capture, fake_presented, &tiny_frame));
    shot_hotkey_set_bounds(&tiny, 0u, 1000u);
    CHECK(!shot_hotkey_take(&tiny, 1u, "shot", 1u) && tiny.refused == 1u && tiny.taken == 0u && tiny_frame.calls == 0u);
    CHECK(count_matching(dir2, "", "") <= 2u && !exists_in(dir2, "shot-001.png") && !exists_in(dir2, "shots.manifest"));
    /* the estimate is the last picture: when the next frame is BIGGER the exact size is checked after the read, the press is
     * refused, its number is not used and nothing is left on disk */
    char dir3[300];
    make_dir(dir3, sizeof dir3, base, "bytes_grow");
    fake_frame grow = frame_of(0u, false, false, 16u, 9u);
    shot_hotkey g;
    CHECK(shot_hotkey_init(&g, dir3, fake_capture, fake_presented, &grow));
    const uint64_t small = shot_png_size(16u, 9u);
    shot_hotkey_set_bounds(&g, 0u, shot_png_size(640u, 480u) + small / 2u);
    CHECK(shot_hotkey_take(&g, 1u, "shot", 1u));
    grow.width = 640u;
    grow.height = 480u;
    CHECK(!shot_hotkey_take(&g, 2u, "shot", 2u));
    CHECK(grow.calls == 2u && g.refused == 1u && g.failed == 0u && g.taken == 1u && g.bytes == small);
    CHECK(count_matching(dir3, "shot-", ".png") == 1u && count_matching(dir3, ".shot-", "") == 0u);
    grow.width = 16u;
    grow.height = 9u;
    CHECK(shot_hotkey_take(&g, 3u, "shot", 3u) && exists_in(dir3, "shot-002.png")); /* the refused press kept its number */
}

static void test_existing_names_skipped(const char *base)
{
    char dir[300];
    make_dir(dir, sizeof dir, base, "exists");
    write_text(dir, "shot-001.png", "KEEP-ONE");
    write_text(dir, "shot-002.png", "KEEP-TWO");
    char link_path[400];
    snprintf(link_path, sizeof link_path, "%s/shot-003.png", dir);
    CHECK(symlink("/nonexistent/target", link_path) == 0); /* a dangling symlink holds the name too */
    fake_frame frame = frame_of(0u, false, false, 16u, 9u);
    shot_hotkey shot;
    CHECK(shot_hotkey_init(&shot, dir, fake_capture, fake_presented, &frame));
    CHECK(shot_hotkey_take(&shot, 1u, "shot", 10u));
    CHECK(shot_hotkey_take(&shot, 2u, "shot", 20u));
    char *one = read_text(dir, "shot-001.png"), *two = read_text(dir, "shot-002.png");
    CHECK(one != NULL && strcmp(one, "KEEP-ONE") == 0 && two != NULL && strcmp(two, "KEEP-TWO") == 0);
    free(one);
    free(two);
    CHECK(exists_in(dir, "shot-004.png") && exists_in(dir, "shot-005.png") && !exists_in(dir, "shot-006.png"));
    char *manifest = read_text(dir, "shots.manifest");
    CHECK(manifest != NULL && strstr(manifest, "shot-004.png poll=10 ") != NULL && strstr(manifest, "shot-005.png poll=20 ") != NULL &&
          strstr(manifest, "shot-001") == NULL);
    free(manifest);
    uint32_t w, h;
    snprintf(link_path, sizeof link_path, "%s/shot-004.png", dir);
    uint8_t *back = png_decode(link_path, &w, &h);
    CHECK(back != NULL && w == 16u && h == 9u);
    free(back);
    /* a new run in the same dir (a fresh counter) continues after the pictures of the first */
    shot_hotkey second;
    CHECK(shot_hotkey_init(&second, dir, fake_capture, fake_presented, &frame));
    CHECK(shot_hotkey_take(&second, 1u, "shot", 30u) && exists_in(dir, "shot-006.png"));
    one = read_text(dir, "shot-001.png");
    CHECK(one != NULL && strcmp(one, "KEEP-ONE") == 0);
    free(one);
}

static void test_atomic_and_failures(const char *base)
{
    char dir[300];
    make_dir(dir, sizeof dir, base, "atomic");
    fake_frame frame = frame_of(2u, false, false, 16u, 9u);
    shot_hotkey shot;
    CHECK(shot_hotkey_init(&shot, dir, fake_capture, fake_presented, &frame));
    CHECK(shot_hotkey_take(&shot, 1u, "shot", 1u));
    CHECK(!shot_hotkey_take(&shot, 2u, "shot", 2u)); /* the capture hook says no (a sink that is not a window) */
    CHECK(shot.taken == 1u && shot.failed == 1u && !exists_in(dir, "shot-002.png"));
    /* a capture that "works" but writes no BMP: failed, nothing left */
    frame.garbage = true;
    CHECK(!shot_hotkey_take(&shot, 3u, "shot", 3u));
    frame.garbage = false;
    CHECK(shot.failed == 2u && !exists_in(dir, "shot-003.png"));
    /* the temp name is taken by a directory: the PNG cannot be written, nothing is renamed, the BMP is removed */
    char blocker[400];
    snprintf(blocker, sizeof blocker, "%s/.shot-004.png.tmp", dir);
    CHECK(mkdir(blocker, 0777) == 0);
    CHECK(!shot_hotkey_take(&shot, 4u, "shot", 4u));
    CHECK(shot.failed == 3u && !exists_in(dir, "shot-004.png"));
    rmdir(blocker);
    CHECK(shot_hotkey_take(&shot, 5u, "shot", 5u) && exists_in(dir, "shot-005.png"));
    /* after successes and failures: only PNGs, the manifest, no .tmp, no .bmp */
    CHECK(count_matching(dir, ".", ".tmp") == 0u && count_matching(dir, ".", ".bmp") == 0u && count_matching(dir, ".shot-", "") == 0u);
    CHECK(count_matching(dir, "shot-", ".png") == 2u);
    char *manifest = read_text(dir, "shots.manifest");
    CHECK(manifest != NULL && count_lines(manifest) == 2u && manifest[strlen(manifest) - 1u] == '\n'); /* whole lines only */
    free(manifest);
    /* a stale temp from a crashed run does not block or leak into the result */
    char stale[400];
    snprintf(stale, sizeof stale, "%s/.shot-006.png.tmp", dir);
    write_text(dir, ".shot-006.png.tmp", "half a PNG");
    CHECK(shot_hotkey_take(&shot, 6u, "shot", 6u) && exists_in(dir, "shot-006.png") && !exists_in(dir, ".shot-006.png.tmp"));
    uint32_t w, h;
    snprintf(stale, sizeof stale, "%s/shot-006.png", dir);
    uint8_t *back = png_decode(stale, &w, &h);
    CHECK(back != NULL && w == 16u && h == 9u);
    free(back);
    /* the final name is taken by someone else between the numbering and the rename: the rename fails, the temp PNG is removed */
    frame.race_dir = dir;
    frame.race_number = 7u;
    CHECK(!shot_hotkey_take(&shot, 7u, "shot", 7u));
    frame.race_dir = NULL;
    CHECK(shot.failed == 4u && shot.taken == 3u && !exists_in(dir, ".shot-007.png.tmp") && count_matching(dir, ".shot-", "") == 0u);
    CHECK(exists_in(dir, "shot-007.png/inner")); /* the other writer's entry is untouched */
    CHECK(shot_hotkey_take(&shot, 8u, "shot", 8u) && exists_in(dir, "shot-008.png"));
    manifest = read_text(dir, "shots.manifest");
    CHECK(manifest != NULL && strstr(manifest, "shot-007") == NULL && count_lines(manifest) == 4u);
    free(manifest);
    /* a sink that is never a window: every press fails soft, the game would go on */
    char dir2[300];
    make_dir(dir2, sizeof dir2, base, "nowindow");
    fake_frame never = frame_of(0u, true, false, 16u, 9u);
    shot_hotkey off;
    CHECK(shot_hotkey_init(&off, dir2, fake_capture, fake_presented, &never));
    CHECK(!shot_hotkey_take(&off, 1u, "shot", 1u) && !shot_hotkey_take(&off, 2u, "shot", 2u) && !shot_hotkey_take(&off, 3u, "shot", 3u));
    CHECK(off.failed == 3u && off.taken == 0u && off.refused == 0u && off.bytes == 0u);
    CHECK(count_matching(dir2, "", ".png") == 0u && !exists_in(dir2, "shots.manifest") && count_matching(dir2, ".shot-", "") == 0u);
    /* an unwritable shot dir (its parent is a regular file): init is fine, every press fails soft, nothing is thrown */
    char afile[400], under[500];
    snprintf(afile, sizeof afile, "%s/afile", base);
    write_text(base, "afile", "I am a file");
    snprintf(under, sizeof under, "%s/shots", afile);
    shot_hotkey bad;
    fake_frame ok_frame = frame_of(0u, false, false, 16u, 9u);
    stderr_begin(base);
    CHECK(shot_hotkey_init(&bad, under, fake_capture, fake_presented, &ok_frame));
    CHECK(!shot_hotkey_take(&bad, 1u, "shot", 1u) && !shot_hotkey_take(&bad, 2u, "shot", 2u));
    char *err = stderr_end(base);
    CHECK(bad.failed == 2u && bad.taken == 0u && bad.bytes == 0u);
    CHECK(err != NULL && strstr(err, "cannot be created") != NULL && strstr(err, "FAILED") != NULL);
    free(err);
    CHECK(!exists_in(base, "afile/shots"));
}

static void test_separate_directories(const char *base)
{
    char hot[300], nested[300];
    make_dir(hot, sizeof hot, base, "hot");
    snprintf(nested, sizeof nested, "%s/deep/er/shots", base); /* created on init (mkdir -p) */
    write_text(hot, "hotkey.1", "1 shot poll=5 ms=5\n");
    write_text(hot, "phase", "menu\n");
    fake_frame frame = frame_of(0u, false, false, 16u, 9u);
    shot_hotkey shot;
    CHECK(shot_hotkey_init(&shot, nested, fake_capture, fake_presented, &frame));
    CHECK(exists_in(base, "deep/er/shots"));
    shot_hotkey_set_phase_dir(&shot, hot);
    CHECK(shot_hotkey_take(&shot, 1u, "shot", 7u));
    CHECK(exists_in(nested, "shot-001.png") && exists_in(nested, "shots.manifest"));
    CHECK(!exists_in(hot, "shot-001.png") && !exists_in(hot, "shots.manifest") && exists_in(hot, "hotkey.1")); /* the hotkey dir is untouched */
    char *manifest = read_text(nested, "shots.manifest");
    CHECK(manifest != NULL && strstr(manifest, "shot-001.png poll=7 present=100 phase=menu label=shot hotkey=1 size=16x9 unix=") != NULL);
    free(manifest);
    /* with no phase file in the hotkey dir, the shot dir's own phase file is used, and without either the phase is "-" */
    write_text(nested, "phase", "inshots\n");
    char phase_path[400];
    snprintf(phase_path, sizeof phase_path, "%s/phase", hot);
    remove(phase_path);
    CHECK(shot_hotkey_take(&shot, 2u, "shot", 8u));
    manifest = read_text(nested, "shots.manifest");
    CHECK(manifest != NULL && strstr(manifest, "shot-002.png poll=8 present=101 phase=inshots ") != NULL);
    free(manifest);
    snprintf(phase_path, sizeof phase_path, "%s/phase", nested);
    remove(phase_path);
    CHECK(shot_hotkey_take(&shot, 3u, "shot", 9u));
    manifest = read_text(nested, "shots.manifest");
    CHECK(manifest != NULL && strstr(manifest, "shot-003.png poll=9 present=102 phase=- ") != NULL);
    free(manifest);
    /* default phase dir = the shot dir */
    shot_hotkey plain;
    char plain_dir[300];
    make_dir(plain_dir, sizeof plain_dir, base, "plain");
    write_text(plain_dir, "phase", "same\n");
    CHECK(shot_hotkey_init(&plain, plain_dir, fake_capture, fake_presented, &frame));
    CHECK(shot_hotkey_take(&plain, 1u, "shot", 1u));
    manifest = read_text(plain_dir, "shots.manifest");
    CHECK(manifest != NULL && strstr(manifest, "phase=same ") != NULL);
    free(manifest);
}

typedef struct {
    unsigned calls, number;
    char label[16];
    uint64_t poll;
    bool result;
} shot_probe;
static bool probe_hook(void *user, unsigned number, const char *label, uint64_t poll)
{
    shot_probe *p = user;
    p->calls++;
    p->number = number;
    snprintf(p->label, sizeof p->label, "%s", label);
    p->poll = poll;
    return p->result;
}

static void test_label_dispatch(void)
{
    CHECK(xinput_hotkey_action_of("shot") == XINPUT_HOTKEY_ACTION_SHOT && xinput_hotkey_action_of("SHOT") == XINPUT_HOTKEY_ACTION_SHOT);
    CHECK(xinput_hotkey_action_of("shots") == XINPUT_HOTKEY_ACTION_NONE);
    hotkey_actions blank;
    hotkey_actions_init(&blank, XINPUT_HOST_GAMEPAD, NULL);
    hotkey_actions_fire(1u, "shot", 10u, &blank); /* no shot hook: refused, never fatal */
    CHECK(blank.shots_taken == 0u && blank.shots_refused == 1u);
    shot_probe probe = {0, 0, "", 0, true};
    const hotkey_action_hooks hooks = {NULL, NULL, &probe, probe_hook};
    hotkey_actions actions;
    hotkey_actions_init(&actions, XINPUT_HOST_KEYBOARD, &hooks);
    hotkey_actions_fire(5u, "Shot", 42u, &actions);
    CHECK(probe.calls == 1u && probe.number == 5u && probe.poll == 42u && strcmp(probe.label, "Shot") == 0 && actions.shots_taken == 1u);
    probe.result = false;
    hotkey_actions_fire(6u, "shot", 43u, &actions);
    CHECK(actions.shots_taken == 1u && actions.shots_refused == 1u);
    hotkey_actions_fire(7u, "advance", 44u, &actions);
    CHECK(probe.calls == 2u);
    xinput_hotkey_spec spec;
    char error[160];
    CHECK(xinput_hotkey_parse(XINPUT_HOTKEY_DEFAULT_PAD_SHOT, &spec, error, sizeof error) && strcmp(spec.label, "shot") == 0);
    CHECK(xinput_hotkey_parse(XINPUT_HOTKEY_DEFAULT_KB_SHOT, &spec, error, sizeof error) && strcmp(spec.label, "shot") == 0);
}

static int remove_entry(const char *path, const struct stat *info, int flag, struct FTW *walk)
{
    (void)info;
    (void)flag;
    (void)walk;
    return remove(path);
}

int main(void)
{
    char dir[] = "/tmp/t1720_shot_XXXXXX";
    if (mkdtemp(dir) == NULL) return 1;
    test_png_roundtrip(dir);
    test_png_size_matches(dir);
    test_bmp_loader(dir);
    test_manifest_format();
    test_take(dir);
    test_defaults_and_summary(dir);
    test_bound_by_count(dir);
    test_bound_by_bytes(dir);
    test_existing_names_skipped(dir);
    test_atomic_and_failures(dir);
    test_separate_directories(dir);
    test_label_dispatch();
    printf("test_shot_hotkey: %d checks, %d failures (scratch dir %s%s)\n", checks, failures, dir, failures == 0 ? ", removed" : ", kept");
    if (failures == 0) nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    return failures == 0 ? 0 : 1;
}
