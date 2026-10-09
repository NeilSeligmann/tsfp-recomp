/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_texture_dirty.h"
#include <float.h>
#include <string.h>
#include "d3d8_guest.h"
#include "kernel_call.h"
#include "d3d8_hle.h"
#include "d3d8_pushbuffer.h"

typedef struct {
    uint32_t stage;
    uint32_t address;
    uint32_t control;
    uint32_t filter;
} stage_packet;

static float read_float(uint32_t address)
{
    const uint32_t bits = d3d8_guest_load32(address);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t prepare_stages(uint32_t dirty, stage_packet packets[4])
{
#if LDBL_MANT_DIG != 64
    d3d8_hle_fatal(0x003DDCB0u, "texture-stage emitter requires x87 extended precision");
#endif
    uint32_t count = 0u;
    for (uint32_t stage = 0u; stage < 4u; stage++) {
        if ((dirty & (1u << stage)) == 0u) continue;
        const uint32_t shadow = 0x003E3AC4u + stage * 0x80u;
        const uint32_t coordinate = d3d8_guest_load32(shadow + 0x6Cu) & 0xFFFFu;
        uint32_t address = d3d8_guest_load32(shadow + 4u) << 8u;
        address |= d3d8_guest_load32(shadow);
        address <<= 8u;
        address |= d3d8_guest_load32(0x003E3E48u + coordinate * 4u);
        address |= d3d8_guest_load32(shadow - 4u);
        uint32_t control = d3d8_guest_load32(shadow + 0x18u) << 26u;
        control |= d3d8_guest_load32(shadow + 0x28u);
        control |= d3d8_guest_load32(shadow + 0x20u);
        control |= 0x3FFC0u;
        if (d3d8_device_load32(0xF88u + stage * 4u) != 0u) control |= 0x40000000u;
        uint32_t minimum = d3d8_guest_load32(shadow + 0xCu);
        uint32_t magnification = d3d8_guest_load32(shadow + 8u);
        uint32_t filter_flags = 0x2000u;
        if (minimum >= 3u || magnification >= 3u) {
            if (minimum <= 3u && magnification <= 3u) {
                const uint32_t anisotropy = d3d8_guest_load32(shadow + 0x1Cu);
                if (anisotropy == 0u) {
                    minimum = magnification = 1u;
                } else {
                    minimum = magnification = 2u;
                    control |= (anisotropy - 1u) << 4u;
                }
            } else {
                if (minimum == 5u || magnification == 5u) filter_flags = 0x4000u;
                filter_flags |= 0x70000u;
                minimum = 2u;
                magnification = 4u;
            }
        }
        /* Only indices 3..8 are the six filter words at the original code/data
         * seam 0x003E1CF4. Other indices read unrelated instructions/data. */
        const uint32_t mip = d3d8_guest_load32(shadow + 0x10u);
        if (minimum < 1u || minimum > 2u || mip > 2u)
            d3d8_hle_fatal(0x003DDCB0u, "unsupported texture filter min=%#x mip=%#x", minimum, mip);
        static const uint32_t filters[6] = {
            0x10000u, 0x30000u, 0x50000u, 0x20000u, 0x40000u, 0x60000u
        };
        const long double bias = (long double)read_float(shadow + 0x14u) +
                                  (long double)read_float(D3D8_DEVICE_BASE + 0x968u);
        const float rounded = (float)(bias * 256.0L + 0.5L);
        int32_t quantized = rounded >= -2147483648.0L && rounded < 2147483648.0L
                            ? (int32_t)rounded : INT32_MIN;
        if (quantized < -4096) quantized = -4096;
        if (quantized > 4095) quantized = 4095;
        const uint32_t format = d3d8_guest_load32(shadow + 0x2Cu);
        const uint32_t convolution = format >= 25u ? 0xC0000000u : d3d8_guest_load32(shadow + 0x24u);
        stage_packet *packet = &packets[count++];
        packet->stage = stage;
        packet->address = address;
        packet->control = control;
        packet->filter = ((uint32_t)quantized & 0x1FFFu) | filters[minimum * 3u + mip - 3u] |
                         (magnification << 24u) | convolution | filter_flags;
    }
    return count;
}

void d3d8_texture_stages_preflight(uint32_t dirty)
{
    stage_packet packets[4];
    (void)prepare_stages(dirty, packets);
}

uint32_t d3d8_emit_texture_stages(uint32_t dirty)
{
    stage_packet packets[4];
    const uint32_t count = prepare_stages(dirty, packets);
    for (uint32_t i = 0u; i < count; i++) {
        const stage_packet *packet = &packets[i];
        /* 0x003DDE2C: every stage packet has its own reservation preamble. */
        uint32_t cursor = d3d8_pushbuffer_begin();
        /* Original cache has bit 30 forced even when this stage is unbound;
         * the emitted control word only sets it for a nonnull texture. */
        d3d8_device_store32(0x774u + packet->stage * 4u, packet->control | 0x40000000u);
        d3d8_guest_store32(cursor, 0x00081B08u + packet->stage * 0x40u);
        d3d8_guest_store32(cursor + 4u, packet->address);
        d3d8_guest_store32(cursor + 8u, packet->control);
        d3d8_guest_store32(cursor + 12u, 0x00041B14u + packet->stage * 0x40u);
        d3d8_guest_store32(cursor + 16u, packet->filter);
        cursor += 20u;
        d3d8_pushbuffer_end(cursor);
    }
    return 0u;
}

static uint32_t float_bits(long double value)
{
    const float stored = (float)value;
    uint32_t bits;
    memcpy(&bits, &stored, 4u);
    return bits;
}
static void fog_span(uint32_t address, uint32_t bytes)
{
    if (kernel_guest_at(address, bytes) == NULL)
        d3d8_hle_fatal(0x003DDEA0u, "fog input/output span is not mapped");
}
static void helper_output_span(uint32_t cursor, uint32_t bytes)
{
    fog_span(cursor, bytes);
    /* Snapshotting overlapping guest inputs would change original sequential
     * stores/copies. Only disjoint command storage is supported. */
    const uint64_t end = (uint64_t)cursor + bytes;
    if ((cursor < 0x00400000u && end > 0x003D0000u) ||
        (cursor < 0x00480000u && end > 0x00470000u) ||
        (cursor < 0x004A2000u && end > 0x004A1000u))
        d3d8_hle_fatal(0x003DDEA0u, "fog helper command/input overlap is not recovered");
}
/* Payload inputs are live after 0x003D5C98's sized reservation. Only the
 * program table and word count are captured before that call. */
static uint32_t build_fog_program(uint32_t words[63], uint32_t count, uint32_t table)
{
    fog_span(D3D8_DEVICE_BASE, 0xF00u);
    fog_span(0x003E3EFCu, 4u);
    fog_span(0x003E3F20u, 4u);
    fog_span(table, count * 4u);
    const uint32_t flags = d3d8_device_load32(8u);
    uint32_t zscale;
    if (d3d8_guest_load32(0x003E3EFCu) == 2u) {
        volatile long double product = (long double)read_float(D3D8_DEVICE_BASE + 0x948u) *
                                      read_float(D3D8_DEVICE_BASE + 0x944u);
        zscale = float_bits(product);
    } else { fog_span(0x00475C78u,4u);zscale = float_bits((long double)read_float(0x00475C78u)); }
    const uint32_t bias_address = (flags & 0x8000u) != 0u && d3d8_guest_load32(0x003E3F20u) != 0u
                                 ? 0x00475CD4u : 0x00475CACu;
    fog_span(bias_address,4u);
    const long double bias = read_float(bias_address);
    volatile long double x = (long double)read_float(D3D8_DEVICE_BASE + 0xEF8u) - bias;
    volatile long double y = (long double)read_float(D3D8_DEVICE_BASE + 0xEFCu) - bias;
    const uint32_t prefix[13] = {0x00041EA4u,0u,0x00200B80u,
        d3d8_device_load32(0x95Cu),d3d8_device_load32(0x960u),d3d8_device_load32(0x948u),
        zscale,float_bits(x),float_bits(y),0u,0u,0x00041E9Cu,0u};
    memcpy(words,prefix,sizeof(prefix));
    uint32_t output=13u;
    for(uint32_t copied=0u;copied<count;) {
        const uint32_t chunk=count-copied>32u?32u:count-copied;
        words[output++]=(chunk<<18u)+0xB00u;
        for(uint32_t i=0u;i<chunk;i++)words[output++]=d3d8_guest_load32(table+(copied+i)*4u);
        copied+=chunk;
    }
    return output;
}
/* `reserve_out` is the size in dwords of the sized reservation 0x003D5C94 makes before the helper writes (T546: the
 * placement plans and performs it, so a refill there is no longer refused). */
static uint32_t prepare_fog_program(uint32_t words[63], uint32_t *declaration_out, uint32_t *reserve_out,
                                    uint32_t *table_out)
{
    *reserve_out = 0u;
    fog_span(D3D8_DEVICE_BASE, 0x798u);
    const uint32_t declaration = d3d8_device_load32(0x794u);
    if ((uint64_t)declaration + 8u > UINT64_C(0x100000000))
        d3d8_hle_fatal(0x003DDEA0u, "fog declaration pointer overflows");
    fog_span(declaration, 8u);
    *declaration_out = declaration;
    if ((d3d8_guest_load32(declaration + 4u) & 2u) == 0u) return 0u;
#if LDBL_MANT_DIG != 64
    d3d8_hle_fatal(0x003DDEA0u, "fog program requires x87 extended precision");
#endif
    fog_span(0x003E3F58u, 4u);
    if (d3d8_guest_load32(0x003E3F58u) != D3D8_DEVICE_BASE)
        d3d8_hle_fatal(0x003DDEA0u, "fog reservation device differs from emitter device");
    fog_span(D3D8_DEVICE_BASE, 0xF00u);
    fog_span(0x003E3E34u, 4u);fog_span(0x003E3EFCu, 4u);fog_span(0x003E3F20u, 4u);
    const uint32_t flags = d3d8_device_load32(8u);
    const uint32_t mode = d3d8_guest_load32(0x003E3E34u);
    const uint32_t count = mode != 0u && (flags & 2u) != 0u ? 44u : 48u;
    const uint32_t table = mode == 0u ? 0x003E1A78u : (flags & 2u) != 0u ? 0x003E19C8u : 0x003E1908u;
    fog_span(table, count * 4u);
    *reserve_out = count + 30u;
    if (table_out != NULL) *table_out = table;
    return build_fog_program(words, count, table);
}
/* Where the sites of the fog emitter land (T546). The vertex-program helper 0x003D5C50 makes the sized reservation
 * 0x003D6B30(count + 30) and writes its packet under it with no limit check of its own; the fog packet follows with its
 * own preamble (0x003DDF80 or 0x003DE017), so a refill falls BETWEEN the two and the packet is written after a real
 * begin(). `helper` is where the helper writes (0 without one), `fog` where the fog packet does. */
typedef struct {
    uint32_t helper;
    uint32_t fog;
} fog_sites;

/* The helper's and the fog packet's storage, and what a refill in between writes, must stay clear of the inputs this port
 * snapshots before it writes (changing one under the original's sequential reads is not recovered). */
static void check_refill_spans(const d3d8_pushbuffer_sim *sim, uint32_t refills_before)
{
    if (sim->refills == refills_before) return;
    if (sim->last_refill.wraps) helper_output_span(sim->last_refill.at, 4u);
    if (sim->last_refill.fence_bytes != 0u)
        helper_output_span(sim->last_refill.fence_begin, sim->last_refill.fence_bytes);
}

static void plan_fog_sites(d3d8_pushbuffer_sim *sim, uint32_t program_words, uint32_t reserve_dwords,
                           uint32_t fog_words, fog_sites *sites)
{
    sites->helper = 0u;
    if (program_words == 0u) {
        /* 0x003DDF80 / 0x003DE017: one preamble, then the whole packet at the resulting cursor. */
        sites->fog = d3d8_pushbuffer_sim_site(sim, 0x003DDEA0u, fog_words * 4u);
        fog_span(sites->fog, fog_words * 4u);
        return;
    }
    if ((uint64_t)sim->limit + 0x200u > UINT32_MAX ||
        (uint64_t)sim->cursor + (uint64_t)reserve_dwords * 4u > UINT32_MAX)
        d3d8_hle_fatal(0x003DDEA0u, "fog reservation arithmetic overflows");
    uint32_t refills = sim->refills;
    (void)d3d8_pushbuffer_sim_reserve(sim, reserve_dwords);
    check_refill_spans(sim, refills);
    sites->helper = d3d8_pushbuffer_sim_write(sim, 0x003DDEA0u, program_words * 4u);
    helper_output_span(sites->helper, program_words * 4u);
    if (fog_words == 0u) return; /* the helper alone (d3d8_emit_fog_vertex_program) */
    refills = sim->refills;
    sites->fog = d3d8_pushbuffer_sim_site(sim, 0x003DDEA0u, fog_words * 4u);
    check_refill_spans(sim, refills);
    helper_output_span(sites->fog, fog_words * 4u);
}

uint32_t d3d8_emit_fog_vertex_program(void)
{
    uint32_t words[63], declaration, reserve, table;
    const uint32_t count=prepare_fog_program(words,&declaration,&reserve,&table);
    if(count==0u)return declaration;
    d3d8_pushbuffer_sim sim=d3d8_pushbuffer_sim_start();
    fog_sites sites;
    plan_fog_sites(&sim,count,reserve,0u,&sites);
    /* 0x003D5C98: the sized reservation, which may roll the ring over (and publishes the cursor first). */
    uint32_t cursor=d3d8_pushbuffer_reserve(reserve);
    if(cursor!=sites.helper)d3d8_hle_fatal(0x003DDEA0u,"fog helper plan disagrees with the reservation");
    (void)build_fog_program(words, reserve - 30u, table);
    for(uint32_t i=0u;i<count;i++)d3d8_guest_store32(cursor+i*4u,words[i]);
    cursor+=count*4u;d3d8_pushbuffer_end(cursor);return cursor;
}
/* T461: 0x003D5C50 as the library's render state helpers (0x8F, 0x98) run it. The plan is read-only and refuses what
 * the run would refuse, over a writer that carries the refill state. Both do nothing for a declaration without flag 2. */
void d3d8_plan_vertex_program_helper(d3d8_pushbuffer_sim *sim)
{
    uint32_t words[63], declaration, reserve;
    const uint32_t count = prepare_fog_program(words, &declaration, &reserve, NULL);
    if (count == 0u) return;
    fog_sites sites;
    plan_fog_sites(sim, count, reserve, 0u, &sites);
}
void d3d8_run_vertex_program_helper(void)
{
    (void)d3d8_emit_fog_vertex_program();
}
/* The words of the whole emitter: the helper's first (`*program_out` of them, `*reserve_out` the dwords of its sized
 * reservation), then the fog packet. Reads and validates every input, writes nothing. */
static uint32_t prepare_fog(uint32_t all_words[75], uint32_t *program_out, uint32_t *reserve_out)
{
    uint32_t declaration;
    const uint32_t program_count=prepare_fog_program(all_words,&declaration,reserve_out,NULL);
    *program_out=program_count;
    uint32_t *words=all_words+program_count;
    fog_span(0x003E3E30u, 24u);
    const bool fallback = d3d8_device_load32(0x784u) == 0u || d3d8_device_load32(0x788u) == 0u;
    uint32_t count;
    if (d3d8_guest_load32(0x003E3E30u) == 0u) {
        words[0] = 0x000402A4u; words[1] = 0u; count = 2u;
    } else {
#if LDBL_MANT_DIG != 64
        d3d8_hle_fatal(0x003DDEA0u, "enabled fog requires x87 extended precision");
#endif
        const uint32_t mode = d3d8_guest_load32(0x003E3E34u);
        uint32_t source = d3d8_guest_load32(0x003E3E44u) == 0u ? 2u : 1u;
        uint32_t method, scale, bias;
        if (mode == 0u) {
            method = 0x2601u; source = 0u; scale = bias = 0x3F800000u;
        } else if (mode == 3u) {
            fog_span(0x00475C78u, 4u); fog_span(0x003E2958u, 4u);
            const long double start = read_float(0x003E3E38u);
            const long double end = read_float(0x003E3E3Cu);
            volatile long double reciprocal;
            /* FUCOMPP + TEST AH,0x44/JNP: equal falls back, unordered divides. */
            if (start == end) reciprocal = read_float(0x003E2958u);
            else {
                volatile long double difference = end - start;
                reciprocal = (long double)read_float(0x00475C78u) / difference;
            }
            volatile long double product = end * reciprocal;
            volatile long double shifted = product + (long double)read_float(0x00475C78u);
            bias = float_bits(shifted);
            scale = float_bits(-reciprocal);
            method = 0x2601u;
        } else {
            const uint32_t constant = mode == 1u ? 0x004A1BB4u : 0x004A1BB0u;
            fog_span(constant, 4u);
            volatile long double product = (long double)read_float(0x003E3E40u) * read_float(constant);
            scale = float_bits(product); bias = 0x3FC00000u;
            method = mode == 1u ? 0x800u : 0x801u;
        }
        const uint32_t packet[9] = {0x000802A0u, source, 1u, 0x0004029Cu, method,
                                    0x000C09C0u, bias, scale, 0u};
        memcpy(words, packet, sizeof(packet)); count = 9u;
    }
    if (fallback) {
        fog_span(0x003E3E5Cu, 4u);
        const bool lighting = d3d8_guest_load32(0x003E3E5Cu) != 0u;
        words[count++] = 0x00080288u;
        words[count++] = d3d8_guest_load32(0x003E3E30u) == 0u ? (lighting ? 14u : 12u)
                        : (lighting ? 0x130E0300u : 0x130C0300u);
        words[count++] = 0x1C80u;
    }
    return count + program_count;
}
void d3d8_fog_preflight(void)
{
    uint32_t words[75], program, reserve;
    const uint32_t total = prepare_fog(words, &program, &reserve);
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    fog_sites sites;
    plan_fog_sites(&sim, program, reserve, total - program, &sites);
}
uint32_t d3d8_emit_fog(void)
{
    uint32_t words[75], program, reserve;
    const uint32_t total = prepare_fog(words, &program, &reserve);
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    fog_sites sites;
    plan_fog_sites(&sim, program, reserve, total - program, &sites);
    uint32_t cursor;
    if (program != 0u) {
        /* 0x003D5C98: the helper's sized reservation, its packet, the cursor it publishes (0x003D5D3A). */
        cursor = d3d8_pushbuffer_reserve(reserve);
        if (cursor != sites.helper) d3d8_hle_fatal(0x003DDEA0u, "fog helper plan disagrees with the reservation");
        for (uint32_t i = 0u; i < program; i++) d3d8_guest_store32(cursor + i * 4u, words[i]);
        cursor += program * 4u;
        d3d8_pushbuffer_end(cursor);
    }
    /* The fog packet's own preamble: a refill after the helper hands over what the helper wrote. */
    cursor = d3d8_pushbuffer_begin();
    if (cursor != sites.fog) d3d8_hle_fatal(0x003DDEA0u, "fog plan disagrees with the roll-over");
    for (uint32_t i = program; i < total; i++) d3d8_guest_store32(cursor + (i - program) * 4u, words[i]);
    cursor += (total - program) * 4u;
    d3d8_pushbuffer_end(cursor);
    return cursor;
}
uint32_t d3d8_emit_disabled_fog(void) { return d3d8_emit_fog(); }

uint32_t d3d8_texture_packet_bytes(uint32_t dirty)
{
    stage_packet packets[4];
    return prepare_stages(dirty, packets) * 20u;
}
uint32_t d3d8_fog_packet_bytes(void)
{
    uint32_t words[75], program, reserve;
    return prepare_fog(words, &program, &reserve) * 4u;
}

void d3d8_plan_texture_stages(uint32_t dirty, d3d8_pushbuffer_sim *sim)
{
    stage_packet packets[4];
    const uint32_t count = prepare_stages(dirty, packets);
    for (uint32_t i = 0u; i < count; i++) d3d8_pushbuffer_sim_site(sim, 0x003DDCB0u, 20u);
}

void d3d8_plan_fog(d3d8_pushbuffer_sim *sim)
{
    uint32_t words[75], program, reserve;
    const uint32_t total = prepare_fog(words, &program, &reserve);
    fog_sites sites;
    plan_fog_sites(sim, program, reserve, total - program, &sites);
}
