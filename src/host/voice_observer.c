/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "voice_observer.h"
#include <inttypes.h>
#include <string.h>

bool voice_observer_target(uint32_t address)
{
    return address == 0x164C40u || address == 0x164310u || address == 0x461C0u ||
           address == 0x46D80u || address == 0x148A90u || address == 0x6EEA0u ||
           address == 0x233D50u || address == 0x236070u || address == 0x164E00u;
}
static bool word(voice_read_fn read, uint32_t address, uint32_t *value)
{
    unsigned char raw[4];
    if (!read(address, raw, sizeof raw)) return false;
    *value = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
    return true;
}
int32_t voice_observer_sample(uint32_t label, voice_read_fn read)
{
    if (label == 0u) return -1;
    for (unsigned bank = 0u; bank < 2u; bank++) {
        const uint32_t address = 0x7E7200u + bank * 16u;
        uint32_t pointer, count, base, sample;
        if (!word(read, address, &pointer) || !word(read, address + 4u, &count) ||
            !word(read, address + 8u, &base)) continue;
        const uint32_t index = label - base;
        /* Retail uses signed subtraction/comparison. Bound pointer arithmetic too. */
        if ((int32_t)index < 0 || (int32_t)index >= (int32_t)count ||
            index > (UINT32_MAX - pointer) / 4u || !word(read, pointer + index * 4u, &sample)) continue;
        if (bank == 1u || sample != UINT32_MAX) return (int32_t)sample;
    }
    return -1;
}
void voice_observer_note(FILE *log, uint32_t callee, uint32_t esp, uint32_t ecx, uint32_t eax,
                         uint64_t ticks, uint64_t hz, uint64_t frames, voice_read_fn read)
{
    if (!voice_observer_target(callee)) return;
    uint32_t args[9] = {0};
    unsigned valid = 0u;
    for (unsigned i = 0u; i < 9u; i++) {
        if (esp <= UINT32_MAX - i * 4u && word(read, esp + i * 4u, &args[i])) valid |= 1u << i;
    }
    unsigned label_arg = 0u;
    if (callee == 0x164C40u) label_arg = 2u;
    else if (callee == 0x461C0u || callee == 0x164310u) label_arg = 1u;
    else if (callee == 0x148A90u) label_arg = 4u;
    flockfile(log);
    fprintf(log, "voice entry va=%08" PRIX32 " ticks=%" PRIu64 " hz=%" PRIu64
            " audio_frames=%" PRIu64 " esp=%08" PRIX32 " ecx=%08" PRIX32 " eax=%08" PRIX32 " valid=%03X stack=",
            callee, ticks, hz, frames, esp, ecx, eax, valid);
    for (unsigned i = 0u; i < 9u; i++) fprintf(log, "%s%08" PRIX32, i == 0u ? "" : ",", args[i]);
    if (label_arg != 0u && (valid & (1u << label_arg)) != 0u)
        fprintf(log, " label=%" PRIu32 " sample_lookup=%" PRId32, args[label_arg], voice_observer_sample(args[label_arg], read));
    if (callee == 0x46D80u && (valid & 2u) != 0u) fprintf(log, " requested_sample=%" PRIu32, args[1]);
    if (callee == 0x148A90u && (valid & 256u) != 0u) fprintf(log, " handle=%08" PRIX32, args[8]);
    if (callee == 0x164C40u && (valid & 2u) != 0u) fprintf(log, " object=%08" PRIX32, args[1]);
    if (callee == 0x236070u || (callee == 0x233D50u && (valid & 2u) != 0u)) {
        const uint32_t instance = callee == 0x236070u ? eax : args[1];
        uint32_t definition = 0u;
        if (word(read, instance, &definition) && definition <= UINT32_MAX - 0xB0u) {
            static const unsigned offsets[] = {8u, 12u, 16u, 20u, 24u, 176u};
            fprintf(log, " event_instance=%08" PRIX32 " definition=%08" PRIX32, instance, definition);
            for (unsigned i = 0u; i < sizeof offsets / sizeof offsets[0]; i++) {
                uint32_t value = 0u;
                if (word(read, definition + offsets[i], &value))
                    fprintf(log, " def_%02X=%08" PRIX32, offsets[i], value);
            }
        }
    }
    if (callee == 0x6EEA0u) {
        uint32_t state = 0u, time_bits = 0u, lines = 0u, count = 0u;
        (void)word(read, 0x70E164u, &state);
        const bool time_valid = state != 0u && state <= UINT32_MAX - 8u && word(read, state + 8u, &time_bits);
        (void)word(read, 0x7BB204u, &lines);
        (void)word(read, 0x6F73CCu, &count);
        fprintf(log, " cutscene_state=%08" PRIX32 " time_valid=%u time_float_bits=%08" PRIX32
                " line_table=%08" PRIX32 " line_count=%" PRIu32, state, (unsigned)time_valid, time_bits, lines, count);
    }
    fprintf(log, " return=UNAVAILABLE\n");
    /* Keep the raw resident descriptors alongside requests, including changed mode/bank pointers. */
    for (unsigned bank = 0u; bank < 2u; bank++) {
        const uint32_t address = 0x7E7200u + bank * 16u;
        uint32_t descriptor[3] = {0};
        unsigned mask = 0u;
        for (unsigned i = 0u; i < 3u; i++) if (word(read, address + i * 4u, &descriptor[i])) mask |= 1u << i;
        fprintf(log, "voice table address=%08" PRIX32 " valid=%X pointer=%08" PRIX32 " count=%" PRIu32 " base=%" PRIu32 "\n",
                address, mask, descriptor[0], descriptor[1], descriptor[2]);
    }
    fflush(log);
    funlockfile(log);
}

/* Private binary receipt: ASCII range header, exactly `written` bytes, newline.
 * A snapshot is refreshed when any resident descriptor changes. Never dereference
 * an unchecked guest pointer and never allocate based on guest metadata. */
void voice_observer_dump(FILE *dump, uint32_t previous[12], voice_read_fn read)
{
    uint32_t current[12] = {0};
    for (unsigned i = 0u; i < 6u; i++)
        (void)word(read, 0x7E7200u + (i / 3u) * 16u + (i % 3u) * 4u, &current[i]);
    (void)word(read, 0x6B7B10u, &current[6]);
    (void)word(read, 0x6B7B14u, &current[7]);
    (void)word(read, 0x6B7B08u, &current[8]);
    (void)word(read, 0x6B7B0Cu, &current[9]);
    (void)word(read, 0x7BB204u, &current[10]);
    (void)word(read, 0x6F73CCu, &current[11]);
    if (memcmp(current, previous, sizeof current) == 0) return;
    memcpy(previous, current, sizeof current);
    flockfile(dump);
    for (unsigned range = 0u; range < 5u; range++) {
        uint32_t pointer, bytes;
        if (range < 2u) {
            pointer = current[range * 3u];
            bytes = current[range * 3u + 1u] <= 524288u ? current[range * 3u + 1u] * 4u : 0u;
        } else if (range == 4u) {
            pointer = current[10];
            bytes = current[11] <= 4096u ? current[11] * 24u : 0u;
        } else {
            pointer = current[6u + (range - 2u) * 2u];
            bytes = current[7u + (range - 2u) * 2u];
        }
        if (pointer == 0u || bytes == 0u || bytes > 2097152u || bytes > UINT32_MAX - pointer) {
            fprintf(dump, "# range=%u pointer=%08" PRIX32 " bytes=%" PRIu32 " refused\n", range, pointer, bytes);
            continue;
        }
        fprintf(dump, "# range=%u pointer=%08" PRIX32 " bytes=%" PRIu32 "\n", range, pointer, bytes);
        uint32_t offset = 0u;
        unsigned char chunk[4096];
        while (offset < bytes) {
            const uint32_t size = bytes - offset < sizeof chunk ? bytes - offset : (uint32_t)sizeof chunk;
            if (!read(pointer + offset, chunk, size)) break;
            fprintf(dump, "# chunk offset=%" PRIu32 " bytes=%" PRIu32 "\n", offset, size);
            if (fwrite(chunk, 1u, size, dump) != size) break;
            fputc('\n', dump);
            offset += size;
        }
        fprintf(dump, "\n# written=%" PRIu32 " complete=%u\n", offset, (unsigned)(offset == bytes));
    }
    fflush(dump);
    funlockfile(dump);
}
