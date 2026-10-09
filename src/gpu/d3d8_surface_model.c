/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_surface_model.h"

#include "d3d8_gpu_memory.h"

#include <stdlib.h>
#include <string.h>

#define PROBE_PIECE 65536u

typedef struct {
    bool valid;
    uint32_t data;
    uint8_t *bytes;
    size_t length;
} byte_surface;

static struct {
    d3d8_surface_model_read_fn read;
    void *context;
    byte_surface surfaces[D3D8_SURFACE_MODEL_BYTE_SURFACES];
} model;

static bool guest_read(uint32_t address, void *out, size_t bytes)
{
    if (model.read != NULL) {
        return model.read(model.context, address, out, bytes);
    }
    return d3d8_gpu_read_guest(NULL, address, out, bytes);
}

void d3d8_surface_model_set_reader(d3d8_surface_model_read_fn read, void *context)
{
    model.read = read;
    model.context = context;
}

void d3d8_surface_model_reset(void)
{
    for (size_t i = 0u; i < D3D8_SURFACE_MODEL_BYTE_SURFACES; i++) {
        free(model.surfaces[i].bytes);
        memset(&model.surfaces[i], 0, sizeof model.surfaces[i]);
    }
}

const char *d3d8_surface_model_status_string(d3d8_surface_model_status status)
{
    switch (status) {
    case D3D8_SURFACE_MODEL_OK: return "ok";
    case D3D8_SURFACE_MODEL_UNREADABLE: return "the guest memory range cannot be read";
    case D3D8_SURFACE_MODEL_ALIAS: return "the range overlaps a kept byte surface of another Data word";
    case D3D8_SURFACE_MODEL_FULL: return "every byte surface slot is taken";
    case D3D8_SURFACE_MODEL_TOO_LARGE: return "the surface is larger than the byte surface bound";
    case D3D8_SURFACE_MODEL_MEMORY: return "out of memory";
    }
    return "unknown";
}

d3d8_surface_probe d3d8_surface_model_probe(uint32_t data, uint64_t bytes)
{
    uint8_t *piece = malloc(PROBE_PIECE);
    if (piece == NULL) {
        return D3D8_SURFACE_UNREADABLE;
    }
    d3d8_surface_probe result = D3D8_SURFACE_ZERO;
    for (uint64_t done = 0u; done < bytes && result == D3D8_SURFACE_ZERO;) {
        const size_t length = bytes - done < PROBE_PIECE ? (size_t)(bytes - done) : PROBE_PIECE;
        if (!guest_read(data + (uint32_t)done, piece, length)) {
            result = D3D8_SURFACE_UNREADABLE;
            break;
        }
        for (size_t i = 0u; i < length; i++) {
            if (piece[i] != 0u) {
                result = D3D8_SURFACE_NONZERO;
                break;
            }
        }
        done += length;
    }
    free(piece);
    return result;
}

d3d8_surface_model_status d3d8_surface_model_read_argb(uint32_t data, uint32_t width, uint32_t rows, uint32_t pitch, uint8_t *rgba)
{
    if (rgba == NULL || width == 0u || rows == 0u || pitch < width * 4u) {
        return D3D8_SURFACE_MODEL_UNREADABLE;
    }
    uint8_t *row = malloc((size_t)width * 4u);
    if (row == NULL) {
        return D3D8_SURFACE_MODEL_MEMORY;
    }
    d3d8_surface_model_status status = D3D8_SURFACE_MODEL_OK;
    for (uint32_t y = 0u; y < rows; y++) {
        if (!guest_read(data + y * pitch, row, (size_t)width * 4u)) {
            status = D3D8_SURFACE_MODEL_UNREADABLE;
            break;
        }
        uint8_t *out = rgba + (size_t)y * width * 4u;
        for (uint32_t x = 0u; x < width; x++) {
            out[x * 4u + 0u] = row[x * 4u + 2u];
            out[x * 4u + 1u] = row[x * 4u + 1u];
            out[x * 4u + 2u] = row[x * 4u + 0u];
            out[x * 4u + 3u] = row[x * 4u + 3u];
        }
    }
    free(row);
    return status;
}

static byte_surface *find_bytes(uint32_t data)
{
    for (size_t i = 0u; i < D3D8_SURFACE_MODEL_BYTE_SURFACES; i++) {
        if (model.surfaces[i].valid && model.surfaces[i].data == data) {
            return &model.surfaces[i];
        }
    }
    return NULL;
}

bool d3d8_surface_model_has_bytes(uint32_t data)
{
    return find_bytes(data) != NULL;
}

static bool overlaps_other(uint32_t data, uint64_t bytes)
{
    for (size_t i = 0u; i < D3D8_SURFACE_MODEL_BYTE_SURFACES; i++) {
        const byte_surface *other = &model.surfaces[i];
        if (other->valid && other->data != data && data < (uint64_t)other->data + other->length &&
            other->data < (uint64_t)data + bytes) {
            return true;
        }
    }
    return false;
}

bool d3d8_surface_model_overlaps_bytes(uint32_t data, uint64_t bytes)
{
    for (size_t i = 0u; i < D3D8_SURFACE_MODEL_BYTE_SURFACES; i++) {
        const byte_surface *other = &model.surfaces[i];
        if (other->valid && data < (uint64_t)other->data + other->length && other->data < (uint64_t)data + bytes) {
            return true;
        }
    }
    return false;
}

d3d8_surface_model_status d3d8_surface_model_acquire(uint32_t data, size_t length, uint8_t **bytes)
{
    if (length == 0u || length > D3D8_SURFACE_MODEL_MAX_BYTES) {
        return D3D8_SURFACE_MODEL_TOO_LARGE;
    }
    if (overlaps_other(data, length)) {
        return D3D8_SURFACE_MODEL_ALIAS;
    }
    byte_surface *slot = find_bytes(data);
    for (size_t i = 0u; slot == NULL && i < D3D8_SURFACE_MODEL_BYTE_SURFACES; i++) {
        if (!model.surfaces[i].valid) {
            slot = &model.surfaces[i];
        }
    }
    if (slot == NULL) {
        return D3D8_SURFACE_MODEL_FULL;
    }
    const size_t held = slot->valid ? slot->length : 0u;
    if (held >= length) {
        *bytes = slot->bytes;
        return D3D8_SURFACE_MODEL_OK;
    }
    uint8_t *grown = realloc(slot->bytes, length);
    if (grown == NULL) {
        return D3D8_SURFACE_MODEL_MEMORY;
    }
    if (!guest_read(data + (uint32_t)held, grown + held, length - held)) {
        if (slot->valid) {
            slot->bytes = grown; /* realloc moved it: keep the pointer, the length stays */
        } else {
            free(grown);
        }
        return D3D8_SURFACE_MODEL_UNREADABLE;
    }
    slot->bytes = grown;
    slot->length = length;
    slot->data = data;
    slot->valid = true;
    *bytes = grown;
    return D3D8_SURFACE_MODEL_OK;
}

d3d8_surface_model_status d3d8_surface_model_read_bytes(uint32_t data, uint8_t *out, size_t length)
{
    if (overlaps_other(data, length)) {
        return D3D8_SURFACE_MODEL_ALIAS;
    }
    if (length != 0u && !guest_read(data, out, length)) {
        return D3D8_SURFACE_MODEL_UNREADABLE;
    }
    const byte_surface *kept = find_bytes(data);
    if (kept != NULL) {
        memcpy(out, kept->bytes, kept->length < length ? kept->length : length);
    }
    return D3D8_SURFACE_MODEL_OK;
}

size_t d3d8_surface_model_byte_surface_count(void)
{
    size_t count = 0u;
    for (size_t i = 0u; i < D3D8_SURFACE_MODEL_BYTE_SURFACES; i++) {
        count += model.surfaces[i].valid ? 1u : 0u;
    }
    return count;
}

size_t d3d8_surface_model_byte_surface_length(uint32_t data)
{
    const byte_surface *kept = find_bytes(data);
    return kept != NULL ? kept->length : 0u;
}
