/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mu_fatx.h"
#include <string.h>
#include <stdlib.h>

#define DIR_END 0xFFu
#define DIR_DELETED 0xE5u
#define ATTR_FILE 0u

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static size_t round_up(size_t value, size_t unit) { return (value + unit - 1u) / unit * unit; }

static bool geometry(size_t size, uint32_t *clusters, size_t *fat_size, bool *fat32)
{
    if (size < FATX_MIN_IMAGE || size > (size_t)0xFFFFFFF0u * FATX_CLUSTER_BYTES) return false;
    uint32_t count = (uint32_t)((size - FATX_SUPERBLOCK_BYTES) / FATX_CLUSTER_BYTES);
    bool wide = count >= 65525u;
    size_t fat = round_up((size_t)(count + 1u) * (wide ? 4u : 2u), FATX_SUPERBLOCK_BYTES);
    count = (uint32_t)((size - FATX_SUPERBLOCK_BYTES - fat) / FATX_CLUSTER_BYTES);
    if (count < 2u) return false;
    *clusters = count; *fat_size = fat; *fat32 = wide;
    return true;
}
static uint32_t fat_get(const fatx_volume *v, uint32_t cluster)
{
    const uint8_t *e = v->image + v->fat_offset + (size_t)cluster * (v->fat32 ? 4u : 2u);
    return v->fat32 ? rd32(e) : (uint32_t)(e[0] | e[1] << 8);
}
static void fat_set(fatx_volume *v, uint32_t cluster, uint32_t value)
{
    uint8_t *e = v->image + v->fat_offset + (size_t)cluster * (v->fat32 ? 4u : 2u);
    if (v->fat32) wr32(e, value); else { e[0] = (uint8_t)value; e[1] = (uint8_t)(value >> 8); }
}
static uint32_t eoc(const fatx_volume *v) { return v->fat32 ? 0xFFFFFFFFu : 0xFFFFu; }
static bool is_eoc(const fatx_volume *v, uint32_t value) { return v->fat32 ? value >= 0xFFFFFFF8u : value >= 0xFFF8u; }
static uint8_t *cluster_at(const fatx_volume *v, uint32_t cluster)
{ return v->image + v->data_offset + (size_t)(cluster - 1u) * FATX_CLUSTER_BYTES; }

fatx_status fatx_format(uint8_t *image, size_t size, uint32_t volume_id)
{
    uint32_t clusters; size_t fat; bool wide;
    if (image == NULL) return FATX_E_ARGUMENT;
    if (!geometry(size, &clusters, &fat, &wide)) return FATX_E_GEOMETRY;
    memset(image, 0, size);
    wr32(image, FATX_MAGIC); wr32(image + 4, volume_id); wr32(image + 8, FATX_CLUSTER_SECTORS); wr32(image + 12, 1u);
    image[16] = 1u;
    fatx_volume v = {image, size, volume_id, FATX_CLUSTER_SECTORS, 1u, clusters, FATX_SUPERBLOCK_BYTES, fat,
                     FATX_SUPERBLOCK_BYTES + fat, wide};
    fat_set(&v, 0u, wide ? 0xFFFFFFF8u : 0xFFF8u);
    fat_set(&v, 1u, eoc(&v));
    memset(cluster_at(&v, 1u), DIR_END, FATX_CLUSTER_BYTES);
    return FATX_OK;
}
fatx_status fatx_open(fatx_volume *v, uint8_t *image, size_t size)
{
    uint32_t clusters; size_t fat; bool wide;
    if (v == NULL || image == NULL) return FATX_E_ARGUMENT;
    if (!geometry(size, &clusters, &fat, &wide)) return FATX_E_GEOMETRY;
    if (rd32(image) != FATX_MAGIC) return FATX_E_BAD_MAGIC;
    if (rd32(image + 8) != FATX_CLUSTER_SECTORS || rd32(image + 12) != 1u) return FATX_E_GEOMETRY;
    *v = (fatx_volume){image, size, rd32(image + 4), FATX_CLUSTER_SECTORS, 1u, clusters, FATX_SUPERBLOCK_BYTES, fat,
                       FATX_SUPERBLOCK_BYTES + fat, wide};
    if (!is_eoc(v, fat_get(v, 1u))) return FATX_E_CORRUPT;
    return FATX_OK;
}
static bool name_ok(const char *name, size_t *length)
{
    if (name == NULL) return false;
    size_t n = strlen(name);
    if (n == 0u || n > FATX_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        const char c = name[i];
        const bool good = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                          strchr("$%'-_@~`!(){}^#&.", c) != NULL;
        if (!good) return false;
    }
    *length = n;
    return true;
}
static bool entry_matches(const uint8_t *e, const char *name, size_t n)
{
    if (e[0] != n) return false;
    for (size_t i = 0u; i < n; i++) {
        unsigned char a = e[2u + i], b = (unsigned char)name[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}
static uint8_t *find_entry(const fatx_volume *v, const char *name, size_t n, uint8_t **free_slot)
{
    uint8_t *dir = cluster_at(v, v->root_cluster);
    if (free_slot != NULL) *free_slot = NULL;
    for (size_t i = 0; i < FATX_CLUSTER_BYTES / FATX_ENTRY_BYTES; i++) {
        uint8_t *e = dir + i * FATX_ENTRY_BYTES;
        if (e[0] == DIR_END) { if (free_slot != NULL && *free_slot == NULL) *free_slot = e; return NULL; }
        if (e[0] == DIR_DELETED) { if (free_slot != NULL && *free_slot == NULL) *free_slot = e; continue; }
        if (entry_matches(e, name, n)) return e;
    }
    return NULL;
}
static void free_chain(fatx_volume *v, uint32_t cluster)
{
    for (uint32_t guard = 0; cluster >= 2u && cluster <= v->cluster_count && guard <= v->cluster_count; guard++) {
        const uint32_t next = fat_get(v, cluster);
        fat_set(v, cluster, 0u);
        if (is_eoc(v, next) || next == 0u) break;
        cluster = next;
    }
}
fatx_status fatx_write_file(fatx_volume *v, const char *name, const void *data, uint32_t length)
{
    size_t n;
    if (v == NULL || (data == NULL && length != 0u)) return FATX_E_ARGUMENT;
    if (!name_ok(name, &n)) return FATX_E_NAME;
    uint8_t *slot;
    uint8_t *existing = find_entry(v, name, n, &slot);
    const uint32_t need = (uint32_t)(((uint64_t)length + FATX_CLUSTER_BYTES - 1u) / FATX_CLUSTER_BYTES);
    if (existing == NULL && slot == NULL) return FATX_E_DIR_FULL;
    /* Space is checked against what replacing would free, so a failed write leaves the old file intact. */
    uint32_t reusable = 0u;
    if (existing != NULL) {
        if ((existing[1] & 0x10u) != 0u) return FATX_E_IS_DIR;
        uint32_t c = rd32(existing + 0x2C);
        if (c != 0u) {
            for (;;) {
                if (c < 2u || c > v->cluster_count || reusable >= v->cluster_count) return FATX_E_CORRUPT;
                reusable++;
                const uint32_t next = fat_get(v, c);
                if (is_eoc(v, next)) break;
                c = next;
            }
        }
    }
    if (need > fatx_free_clusters(v) + reusable) return FATX_E_NO_SPACE;
    if (existing != NULL) { free_chain(v, rd32(existing + 0x2C)); slot = existing; }
    uint32_t first = 0u, previous = 0u, cursor = 2u;
    for (uint32_t i = 0; i < need; i++) {
        while (cursor <= v->cluster_count && fat_get(v, cursor) != 0u) cursor++;
        if (cursor > v->cluster_count) return FATX_E_CORRUPT;
        fat_set(v, cursor, eoc(v));
        if (previous != 0u) fat_set(v, previous, cursor); else first = cursor;
        const uint32_t offset = i * FATX_CLUSTER_BYTES;
        const uint32_t chunk = length - offset < FATX_CLUSTER_BYTES ? length - offset : FATX_CLUSTER_BYTES;
        memset(cluster_at(v, cursor), 0, FATX_CLUSTER_BYTES);
        memcpy(cluster_at(v, cursor), (const uint8_t *)data + offset, chunk);
        previous = cursor++;
    }
    memset(slot, 0, FATX_ENTRY_BYTES);
    slot[0] = (uint8_t)n; slot[1] = ATTR_FILE; memcpy(slot + 2, name, n);
    memset(slot + 2 + n, 0xFF, FATX_NAME_MAX - n);
    wr32(slot + 0x2C, first); wr32(slot + 0x30, length);
    return FATX_OK;
}
fatx_status fatx_read_file(const fatx_volume *v, const char *name, void *out, uint32_t capacity, uint32_t *length)
{
    size_t n;
    if (v == NULL || length == NULL || (out == NULL && capacity != 0u)) return FATX_E_ARGUMENT;
    if (!name_ok(name, &n)) return FATX_E_NAME;
    const uint8_t *e = find_entry(v, name, n, NULL);
    if (e == NULL) return FATX_E_NOT_FOUND;
    if ((e[1] & 0x10u) != 0u) return FATX_E_IS_DIR;
    const uint32_t size = rd32(e + 0x30);
    uint32_t cluster = rd32(e + 0x2C), copied = 0u;
    *length = size;
    for (uint32_t g = 0; copied < size; g++) {
        if (cluster < 2u || cluster > v->cluster_count || g > v->cluster_count) return FATX_E_CORRUPT;
        const uint32_t chunk = size - copied < FATX_CLUSTER_BYTES ? size - copied : FATX_CLUSTER_BYTES;
        if (copied < capacity) memcpy((uint8_t *)out + copied, cluster_at(v, cluster), chunk < capacity - copied ? chunk : capacity - copied);
        copied += chunk;
        const uint32_t next = fat_get(v, cluster);
        if (copied >= size) break;
        if (is_eoc(v, next)) return FATX_E_CORRUPT;
        cluster = next;
    }
    return FATX_OK;
}
fatx_status fatx_remove_file(fatx_volume *v, const char *name)
{
    size_t n;
    if (v == NULL) return FATX_E_ARGUMENT;
    if (!name_ok(name, &n)) return FATX_E_NAME;
    uint8_t *e = find_entry(v, name, n, NULL);
    if (e == NULL) return FATX_E_NOT_FOUND;
    if ((e[1] & 0x10u) != 0u) return FATX_E_IS_DIR;
    free_chain(v, rd32(e + 0x2C));
    e[0] = DIR_DELETED;
    return FATX_OK;
}
uint32_t fatx_file_count(const fatx_volume *v)
{
    uint32_t count = 0u;
    const uint8_t *dir = cluster_at(v, v->root_cluster);
    for (size_t i = 0; i < FATX_CLUSTER_BYTES / FATX_ENTRY_BYTES; i++) {
        const uint8_t b = dir[i * FATX_ENTRY_BYTES];
        if (b == DIR_END) break;
        if (b != DIR_DELETED) count++;
    }
    return count;
}
uint32_t fatx_free_clusters(const fatx_volume *v)
{
    uint32_t free_count = 0u;
    for (uint32_t c = 2u; c <= v->cluster_count; c++) if (fat_get(v, c) == 0u) free_count++;
    return free_count;
}
const char *fatx_status_name(fatx_status s)
{
    switch (s) {
    case FATX_OK: return "ok"; case FATX_E_ARGUMENT: return "argument"; case FATX_E_BAD_MAGIC: return "bad-magic";
    case FATX_E_GEOMETRY: return "geometry"; case FATX_E_NOT_FOUND: return "not-found"; case FATX_E_NO_SPACE: return "no-space";
    case FATX_E_DIR_FULL: return "dir-full"; case FATX_E_NAME: return "name"; case FATX_E_CORRUPT: return "corrupt";
    case FATX_E_NOT_DIR: return "not-dir"; case FATX_E_IS_DIR: return "is-dir"; case FATX_E_EXISTS: return "exists";
    case FATX_E_PATH_NOT_FOUND: return "path-not-found";
    }
    return "?";
}

/* Single-cluster directories, as in the existing root model. Walk only explicit
 * existing parents, never invent missing paths or interpret dot traversal. */
static fatx_status path_parent(const fatx_volume *v, const char *path, fatx_volume *parent, char leaf[FATX_NAME_MAX + 1u])
{
    if (v == NULL || path == NULL || strlen(path) > 1024u) return FATX_E_ARGUMENT;
    *parent = *v;
    while (*path == '\\' || *path == '/') path++;
    if (*path == '\0') { leaf[0] = '\0'; return FATX_OK; }
    for (unsigned depth = 0u; depth < 32u; depth++) {
        size_t n = 0u;
        while (path[n] != '\0' && path[n] != '\\' && path[n] != '/') {
            if (n == FATX_NAME_MAX) return FATX_E_NAME;
            leaf[n] = path[n]; n++;
        }
        leaf[n] = '\0';
        size_t checked = 0u;
        if (!name_ok(leaf, &checked) || strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0) return FATX_E_NAME;
        path += n;
        while (*path == '\\' || *path == '/') path++;
        if (*path == '\0') return FATX_OK;
        const uint8_t *entry = find_entry(parent, leaf, n, NULL);
        if (entry == NULL) return FATX_E_PATH_NOT_FOUND;
        if ((entry[1] & 0x10u) == 0u) return FATX_E_NOT_DIR;
        const uint32_t cluster = rd32(entry + 0x2C);
        if (cluster < 2u || cluster > v->cluster_count || !is_eoc(v, fat_get(v, cluster))) return FATX_E_CORRUPT;
        parent->root_cluster = cluster;
    }
    return FATX_E_NAME;
}
fatx_status fatx_stat_path(const fatx_volume *v, const char *path, bool *directory, uint32_t *size)
{
    if (directory == NULL || size == NULL) return FATX_E_ARGUMENT;
    fatx_volume parent; char leaf[FATX_NAME_MAX + 1u];
    fatx_status result = path_parent(v, path, &parent, leaf);
    if (result != FATX_OK) return result;
    if (leaf[0] == '\0') { *directory = true; *size = 0u; return FATX_OK; }
    const uint8_t *entry = find_entry(&parent, leaf, strlen(leaf), NULL);
    if (entry == NULL) return FATX_E_NOT_FOUND;
    *directory = (entry[1] & 0x10u) != 0u;
    *size = *directory ? 0u : rd32(entry + 0x30);
    return FATX_OK;
}
fatx_status fatx_mkdir_path(fatx_volume *v, const char *path)
{
    fatx_volume parent; char leaf[FATX_NAME_MAX + 1u];
    fatx_status result = path_parent(v, path, &parent, leaf);
    if (result != FATX_OK) return result;
    if (leaf[0] == '\0') return FATX_E_EXISTS;
    uint8_t *slot = NULL;
    if (find_entry(&parent, leaf, strlen(leaf), &slot) != NULL) return FATX_E_EXISTS;
    if (slot == NULL) return FATX_E_DIR_FULL;
    uint32_t cluster = 2u;
    while (cluster <= v->cluster_count && fat_get(v, cluster) != 0u) cluster++;
    if (cluster > v->cluster_count) return FATX_E_NO_SPACE;
    fat_set(v, cluster, eoc(v));
    memset(cluster_at(v, cluster), DIR_END, FATX_CLUSTER_BYTES);
    memset(slot, 0, FATX_ENTRY_BYTES); slot[0] = (uint8_t)strlen(leaf); slot[1] = 0x10u;
    memcpy(slot + 2, leaf, strlen(leaf)); wr32(slot + 0x2C, cluster);
    return FATX_OK;
}
fatx_status fatx_write_path(fatx_volume *v, const char *path, const void *data, uint32_t size)
{
    fatx_volume parent; char leaf[FATX_NAME_MAX + 1u];
    fatx_status result = path_parent(v, path, &parent, leaf);
    return result == FATX_OK ? fatx_write_file(&parent, leaf, data, size) : result;
}
fatx_status fatx_read_range(const fatx_volume *v, const char *path, uint64_t offset, void *out, uint32_t count, uint32_t *read)
{
    if (read == NULL || (count != 0u && out == NULL)) return FATX_E_ARGUMENT;
    *read = 0u;
    fatx_volume parent; char leaf[FATX_NAME_MAX + 1u];
    fatx_status result = path_parent(v, path, &parent, leaf);
    if (result != FATX_OK) return result;
    const uint8_t *entry = find_entry(&parent, leaf, strlen(leaf), NULL);
    if (entry == NULL) return FATX_E_NOT_FOUND;
    if ((entry[1] & 0x10u) != 0u) return FATX_E_IS_DIR;
    const uint32_t size = rd32(entry + 0x30);
    if (offset >= size || count == 0u) return FATX_OK;
    const uint32_t wanted = (uint64_t)count > size - offset ? (uint32_t)(size - offset) : count;
    uint32_t cluster = rd32(entry + 0x2C), done = 0u;
    uint64_t position = 0u;
    for (uint32_t guard = 0u; done < wanted; guard++) {
        if (cluster < 2u || cluster > v->cluster_count || guard > v->cluster_count) return FATX_E_CORRUPT;
        if (offset < position + FATX_CLUSTER_BYTES) {
            const uint32_t start = (uint32_t)(offset > position ? offset - position : 0u);
            const uint32_t part = wanted - done < FATX_CLUSTER_BYTES - start ? wanted - done : FATX_CLUSTER_BYTES - start;
            memcpy((uint8_t *)out + done, cluster_at(v, cluster) + start, part); done += part;
        }
        position += FATX_CLUSTER_BYTES;
        if (done < wanted) { cluster = fat_get(v, cluster); if (is_eoc(v, cluster)) return FATX_E_CORRUPT; }
    }
    *read = done;
    return FATX_OK;
}
fatx_status fatx_resize_path(fatx_volume *v, const char *path, uint64_t size, uint64_t offset, const void *data, uint32_t count)
{
    if (v == NULL || size > UINT32_MAX || size > v->size || offset > size || count > size - offset || (count != 0u && data == NULL)) return FATX_E_ARGUMENT;
    fatx_volume parent; char leaf[FATX_NAME_MAX + 1u];
    fatx_status result = path_parent(v, path, &parent, leaf);
    if (result != FATX_OK) return result;
    bool directory = false; uint32_t old_size = 0u;
    result = fatx_stat_path(v, path, &directory, &old_size);
    if (result != FATX_OK) return result;
    if (directory) return FATX_E_IS_DIR;
    uint8_t *bytes = calloc(1u, size == 0u ? 1u : (size_t)size);
    if (bytes == NULL) return FATX_E_NO_SPACE;
    uint32_t ignored = 0u;
    result = fatx_read_file(&parent, leaf, bytes, (uint32_t)size, &ignored);
    if (result == FATX_OK) {
        if (count != 0u) memcpy(bytes + offset, data, count);
        result = fatx_write_file(&parent, leaf, bytes, (uint32_t)size);
    }
    free(bytes);
    return result;
}

/* FATX directory fields are decoded from the image. Gregorian/zero-date conversion
 * is a host format model, not a measured Xbox kernel timestamp policy. */
static bool fatx_timestamp(const uint8_t *field, uint64_t *out)
{
    const unsigned time = field[0] | (unsigned)field[1] << 8;
    const unsigned date = field[2] | (unsigned)field[3] << 8;
    if (date == 0u) { *out = 0u; return time == 0u; }
    const unsigned year = 2000u + (date >> 9), month = (date >> 5) & 15u, day = date & 31u;
    const unsigned hour = time >> 11, minute = (time >> 5) & 63u, second = (time & 31u) * 2u;
    static const unsigned month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    const bool leap = year % 4u == 0u && (year % 100u != 0u || year % 400u == 0u);
    if (month < 1u || month > 12u || day < 1u || day > month_days[month - 1u] + (month == 2u && leap) ||
        hour > 23u || minute > 59u || second > 59u) return false;
    uint64_t days = (uint64_t)(year - 1601u) * 365u + (year - 1u) / 4u - (year - 1u) / 100u + (year - 1u) / 400u - 388u;
    for (unsigned m = 1u; m < month; m++) days += month_days[m - 1u] + (m == 2u && leap);
    days += day - 1u;
    *out = (days * 86400u + hour * 3600u + minute * 60u + second) * 10000000u;
    return true;
}
fatx_status fatx_directory_entry_at(const fatx_volume *v, const char *path, unsigned index,
                                   fatx_directory_entry *out, bool *end)
{
    if (out == NULL || end == NULL) return FATX_E_ARGUMENT;
    fatx_volume directory; char leaf[FATX_NAME_MAX + 1u];
    fatx_status result = path_parent(v, path, &directory, leaf);
    if (result != FATX_OK) return result;
    if (leaf[0] != '\0') {
        const uint8_t *entry = find_entry(&directory, leaf, strlen(leaf), NULL);
        if (entry == NULL) return FATX_E_NOT_FOUND;
        if ((entry[1] & 0x10u) == 0u) return FATX_E_NOT_DIR;
        const uint32_t cluster = rd32(entry + 0x2C);
        if (cluster < 2u || cluster > v->cluster_count || !is_eoc(v, fat_get(v, cluster))) return FATX_E_CORRUPT;
        directory.root_cluster = cluster;
    }
    memset(out, 0, sizeof(*out)); *end = index >= FATX_CLUSTER_BYTES / FATX_ENTRY_BYTES;
    if (*end) return FATX_OK;
    const uint8_t *entry = cluster_at(&directory, directory.root_cluster) + index * FATX_ENTRY_BYTES;
    if (entry[0] == DIR_END || entry[0] == 0u) { *end = true; return FATX_OK; }
    if (entry[0] == DIR_DELETED) return FATX_OK;
    if (entry[0] == 0u || entry[0] > FATX_NAME_MAX || memchr(entry + 2, 0, entry[0]) != NULL) return FATX_E_CORRUPT;
    memcpy(out->name, entry + 2, entry[0]); out->attributes = entry[1];
    out->directory = (entry[1] & 0x10u) != 0u;
    out->size = out->directory ? 0u : rd32(entry + 0x30);
    uint32_t cluster = rd32(entry + 0x2C), clusters = 0u;
    if (cluster != 0u) {
        for (;;) {
            if (cluster < 2u || cluster > v->cluster_count || clusters >= v->cluster_count) return FATX_E_CORRUPT;
            clusters++; const uint32_t next = fat_get(v, cluster);
            if (is_eoc(v, next)) break;
            cluster = next;
        }
    }
    out->allocation = (uint64_t)clusters * FATX_CLUSTER_BYTES;
    if (out->size > out->allocation || (out->directory && clusters != 1u)) return FATX_E_CORRUPT;
    if (!fatx_timestamp(entry + 0x34, &out->write_time) ||
        !fatx_timestamp(entry + 0x38, &out->creation_time) ||
        !fatx_timestamp(entry + 0x3C, &out->access_time)) return FATX_E_CORRUPT;
    return FATX_OK;
}
