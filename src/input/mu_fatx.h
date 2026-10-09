/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MU_FATX_H
#define MU_FATX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* FATX volume on a flat byte image (T1085). Layout is the public FATX format: 4096-byte superblock
 * ("FATX", volume id, sectors per cluster, root cluster, FAT copies), then one FAT, then 16 KiB clusters.
 * FAT16 below 65525 clusters, FAT32 above. Directory entries are 64 bytes. Scope: bounded single-cluster directories
 * (256 entries), explicit parent paths and files with cluster chains. Timestamps beyond a fixed stamp and
 * multi-cluster directories are NOT modelled. Confidence: format INFERRED from public FATX documentation,
 * not measured against an xemu-formatted volume (see docs/t1085-mu-fatx.md). */
#define FATX_SECTOR_BYTES 512u
#define FATX_SUPERBLOCK_BYTES 4096u
#define FATX_CLUSTER_SECTORS 32u
#define FATX_CLUSTER_BYTES (FATX_CLUSTER_SECTORS * FATX_SECTOR_BYTES)
#define FATX_NAME_MAX 42u
#define FATX_ENTRY_BYTES 64u
#define FATX_MAGIC 0x58544146u
#define FATX_MIN_IMAGE (FATX_SUPERBLOCK_BYTES * 2u + FATX_CLUSTER_BYTES * 2u)

typedef enum {
    FATX_OK = 0,
    FATX_E_ARGUMENT,
    FATX_E_BAD_MAGIC,
    FATX_E_GEOMETRY,
    FATX_E_NOT_FOUND,
    FATX_E_NO_SPACE,
    FATX_E_DIR_FULL,
    FATX_E_NAME,
    FATX_E_CORRUPT,
    FATX_E_NOT_DIR, FATX_E_IS_DIR, FATX_E_EXISTS, FATX_E_PATH_NOT_FOUND,
} fatx_status;

typedef struct {
    uint8_t *image;
    size_t size;
    uint32_t volume_id;
    uint32_t sectors_per_cluster;
    uint32_t root_cluster;
    uint32_t cluster_count;
    size_t fat_offset;
    size_t fat_size;
    size_t data_offset;
    bool fat32;
} fatx_volume;

/** Lay down an empty volume over the whole image (erases it). */
fatx_status fatx_format(uint8_t *image, size_t size, uint32_t volume_id);
/** Validate the superblock and geometry of an existing image. Never writes. */
fatx_status fatx_open(fatx_volume *volume, uint8_t *image, size_t size);
/** Create or replace a root file. */
fatx_status fatx_write_file(fatx_volume *volume, const char *name, const void *data, uint32_t length);
/** Read a root file into `out` (capacity `capacity`), `*length` is the full file size. */
fatx_status fatx_read_file(const fatx_volume *volume, const char *name, void *out, uint32_t capacity,
                           uint32_t *length);
fatx_status fatx_remove_file(fatx_volume *volume, const char *name);
/** Number of live root entries, or 0xFFFFFFFF if the directory is corrupt. */
uint32_t fatx_file_count(const fatx_volume *volume);
/** Free clusters in the FAT. */
uint32_t fatx_free_clusters(const fatx_volume *volume);
/* Explicit path model: bounded single-cluster directories, existing parents only. */
fatx_status fatx_stat_path(const fatx_volume *volume, const char *path, bool *directory, uint32_t *size);
fatx_status fatx_mkdir_path(fatx_volume *volume, const char *path);
fatx_status fatx_write_path(fatx_volume *volume, const char *path, const void *data, uint32_t size);
fatx_status fatx_read_range(const fatx_volume *volume, const char *path, uint64_t offset, void *out, uint32_t count, uint32_t *read);
fatx_status fatx_resize_path(fatx_volume *volume, const char *path, uint64_t size, uint64_t offset, const void *data, uint32_t count);
typedef struct {
    char name[FATX_NAME_MAX + 1u];
    bool directory;
    uint8_t attributes;
    uint32_t size;
    uint64_t allocation, creation_time, access_time, write_time;
} fatx_directory_entry;
/* Physical directory slot: deleted yields an empty name, end marks 0xFF/boundary.
 * Corrupt entries/chains/timestamps are refused; never mutates the image. */
fatx_status fatx_directory_entry_at(const fatx_volume *volume, const char *path, unsigned index,
                                   fatx_directory_entry *out, bool *end);
const char *fatx_status_name(fatx_status status);
#endif
