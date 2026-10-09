/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Xbox kernel crypto exports. See kernel_crypto.h for the provenance argument,
 * the ordinal resolution and the arity evidence.
 */

#include "kernel_crypto.h"

#include "kernel_call.h"
#include "kernel_hle.h"
#include "nt_status.h"

#include <string.h>

/* Ordinals, read out of tools/kernel_ordinals.py rather than recalled. */
#define ORD_XcSHAInit 335u
#define ORD_XcSHAUpdate 336u
#define ORD_XcSHAFinal 337u
#define ORD_XcRC4Key 338u
#define ORD_XcRC4Crypt 339u
#define ORD_XcHMAC 340u
#define ORD_XcDESKeyParity 346u

size_t kernel_crypto_rc4_key_span(uint32_t key_length)
{
    const uint8_t length = (uint8_t)key_length;
    return length != 0u ? length : 256u;
}

bool kernel_crypto_rc4_key(uint8_t state[KERNEL_RC4_STATE_BYTES],
                            const uint8_t *key, uint32_t key_length)
{
    if (state == NULL || key == NULL) return false;
    const size_t span = kernel_crypto_rc4_key_span(key_length);
    /* The order matters when the key overlaps the state, including counters. */
    state[256] = 0u;
    state[257] = 0u;
    for (unsigned i = 0u; i < 256u; i++) state[i] = (uint8_t)i;
    uint8_t j = 0u;
    size_t key_index = 0u;
    for (unsigned i = 0u; i < 256u; i++) {
        const uint8_t first = state[i];
        j = (uint8_t)(j + first + key[key_index]);
        state[i] = state[j];
        state[j] = first;
        if (++key_index == span) key_index = 0u;
    }
    return true;
}

bool kernel_crypto_rc4_crypt(uint8_t state[KERNEL_RC4_STATE_BYTES],
                              uint8_t *data, uint32_t length)
{
    if (state == NULL || (length != 0u && data == NULL)) return false;
    uint8_t i = state[256], j = state[257];
    uint32_t n = 0u;
    /* Measured kernel fast path: initially aligned input is prefetched in
     * four-byte groups before their permutation updates, then committed once.
     * Initially unaligned input stays on the byte path for the entire call.
     * This ordering matters when input aliases the permutation itself. */
    if (((uintptr_t)data & 3u) == 0u) {
        while (length - n >= 4u) {
            uint8_t input[4], stream[4];
            memcpy(input, data + n, sizeof(input));
            for (unsigned k = 0u; k < 4u; k++) {
                i = (uint8_t)(i + 1u);
                const uint8_t first = state[i];
                j = (uint8_t)(j + first);
                state[i] = state[j];
                state[j] = first;
                stream[k] = state[(uint8_t)(state[i] + state[j])];
            }
            for (unsigned k = 0u; k < 4u; k++) input[k] ^= stream[k];
            memcpy(data + n, input, sizeof(input));
            n += 4u;
        }
    }
    for (; n < length; n++) {
        i = (uint8_t)(i + 1u);
        const uint8_t first = state[i];
        j = (uint8_t)(j + first);
        state[i] = state[j];
        state[j] = first;
        data[n] ^= state[(uint8_t)(state[i] + state[j])];
    }
    /* Must follow data XOR, including an input alias of the counter bytes. */
    state[256] = i;
    state[257] = j;
    return true;
}

uint32_t kernel_crypto_rc4_key_guest(uint32_t state_address, uint32_t length,
                                     uint32_t key_address)
{
    uint8_t *state = kernel_guest_at(state_address, KERNEL_RC4_STATE_BYTES);
    const uint8_t *key = kernel_guest_at(key_address, kernel_crypto_rc4_key_span(length));
    if (state == NULL || key == NULL) return STATUS_INVALID_PARAMETER;
    (void)kernel_crypto_rc4_key(state, key, length);
    return STATUS_SUCCESS;
}

uint32_t kernel_crypto_rc4_crypt_guest(uint32_t state_address, uint32_t length,
                                       uint32_t data_address)
{
    uint8_t *state = kernel_guest_at(state_address, KERNEL_RC4_STATE_BYTES);
    uint8_t *data = length != 0u ? kernel_guest_at(data_address, length) : NULL;
    if (state == NULL || (length != 0u && data == NULL)) return STATUS_INVALID_PARAMETER;
    (void)kernel_crypto_rc4_crypt(state, data, length);
    return STATUS_SUCCESS;
}

static uint32_t hle_xc_rc4_key(void *context)
{
    uint32_t state, length, key;
    if (!kernel_frame_arg(context, 0u, &state) || !kernel_frame_arg(context, 1u, &length) ||
        !kernel_frame_arg(context, 2u, &key)) return STATUS_INVALID_PARAMETER;
    return kernel_crypto_rc4_key_guest(state, length, key);
}

static uint32_t hle_xc_rc4_crypt(void *context)
{
    uint32_t state, length, data;
    if (!kernel_frame_arg(context, 0u, &state) || !kernel_frame_arg(context, 1u, &length) ||
        !kernel_frame_arg(context, 2u, &data)) return STATUS_INVALID_PARAMETER;
    return kernel_crypto_rc4_crypt_guest(state, length, data);
}

/* ======================================================================
 * SHA-1, from FIPS 180-1. Section numbers refer to that publication.
 * ====================================================================== */

/*
 * The four round constants of FIPS 180-1 section 5. Named rather than inlined so a
 * wrong one is a single visible edit: this is a mutation target, because a wrong
 * constant still produces a well-formed 20-byte digest and only a published vector
 * can tell the difference.
 */
#define SHA1_K0 0x5A827999u
#define SHA1_K1 0x6ED9EBA1u
#define SHA1_K2 0x8F1BBCDCu
#define SHA1_K3 0xCA62C1D6u

/* FIPS 180-1 section 7: the five initial chaining values. */
#define SHA1_H0 0x67452301u
#define SHA1_H1 0xEFCDAB89u
#define SHA1_H2 0x98BADCFEu
#define SHA1_H3 0x10325476u
#define SHA1_H4 0xC3D2E1F0u

static uint32_t rotl32(uint32_t value, unsigned bits)
{
    /* bits is never 0 or 32 at any call below, so the shifts stay defined. */
    return (uint32_t)((value << bits) | (value >> (32u - bits)));
}

void kernel_crypto_sha1_reset(kernel_sha1_state *state)
{
    if (!state) {
        return;
    }
    state->chain[0] = SHA1_H0;
    state->chain[1] = SHA1_H1;
    state->chain[2] = SHA1_H2;
    state->chain[3] = SHA1_H3;
    state->chain[4] = SHA1_H4;
    state->bits = 0u;
    state->pending = 0u;
    memset(state->block, 0, sizeof state->block);
}

/* FIPS 180-1 section 7, the 80-round compression of one 512-bit block. */
static void sha1_compress(kernel_sha1_state *state, const uint8_t block[64])
{
    uint32_t w[80];

    /* Section 5: each 32-bit word of the block is BIG-ENDIAN. */
    for (unsigned i = 0u; i < 16u; i++) {
        w[i] = (uint32_t)((uint32_t)block[i * 4u + 0u] << 24) |
               (uint32_t)((uint32_t)block[i * 4u + 1u] << 16) |
               (uint32_t)((uint32_t)block[i * 4u + 2u] << 8) |
               (uint32_t)block[i * 4u + 3u];
    }
    /* Section 7 step (b): the message schedule expansion, rotated left by ONE. */
    for (unsigned i = 16u; i < 80u; i++) {
        w[i] = rotl32(w[i - 3u] ^ w[i - 8u] ^ w[i - 14u] ^ w[i - 16u], 1u);
    }

    uint32_t a = state->chain[0];
    uint32_t b = state->chain[1];
    uint32_t c = state->chain[2];
    uint32_t d = state->chain[3];
    uint32_t e = state->chain[4];

    for (unsigned i = 0u; i < 80u; i++) {
        uint32_t f;
        uint32_t k;
        if (i < 20u) {
            f = (uint32_t)((b & c) | ((~b) & d));
            k = SHA1_K0;
        } else if (i < 40u) {
            f = (uint32_t)(b ^ c ^ d);
            k = SHA1_K1;
        } else if (i < 60u) {
            f = (uint32_t)((b & c) | (b & d) | (c & d));
            k = SHA1_K2;
        } else {
            f = (uint32_t)(b ^ c ^ d);
            k = SHA1_K3;
        }
        const uint32_t t = (uint32_t)(rotl32(a, 5u) + f + e + k + w[i]);
        e = d;
        d = c;
        c = rotl32(b, 30u);
        b = a;
        a = t;
    }

    state->chain[0] += a;
    state->chain[1] += b;
    state->chain[2] += c;
    state->chain[3] += d;
    state->chain[4] += e;
}

void kernel_crypto_sha1_absorb(kernel_sha1_state *state, const void *data, size_t length)
{
    if (!state || (!data && length != 0u)) {
        return;
    }
    const uint8_t *in = (const uint8_t *)data;

    /* Counted in BITS, as FIPS 180-1 section 4 specifies, and 64 bits wide so a
     * message longer than 512 MB does not silently wrap the way a 32-bit byte
     * counter would. */
    state->bits += (uint64_t)length * 8u;

    /* Top up a partially filled block first: this is what makes the streaming
     * XcSHAUpdate form agree with a single-shot hash of the concatenation, which
     * is the property the test suite checks by splitting a vector at every
     * boundary from 0 to its length. */
    if (state->pending != 0u) {
        size_t want = 64u - state->pending;
        if (want > length) {
            want = length;
        }
        memcpy(state->block + state->pending, in, want);
        state->pending += want;
        in += want;
        length -= want;
        if (state->pending == 64u) {
            sha1_compress(state, state->block);
            state->pending = 0u;
        }
    }

    while (length >= 64u) {
        sha1_compress(state, in);
        in += 64u;
        length -= 64u;
    }

    if (length != 0u) {
        memcpy(state->block, in, length);
        state->pending = length;
    }
}

void kernel_crypto_sha1_squeeze(kernel_sha1_state *state, uint8_t digest[20])
{
    if (!state || !digest) {
        return;
    }

    /* The length must be captured BEFORE the padding is absorbed, because the
     * padding is not part of the message. */
    const uint64_t bits = state->bits;

    /* FIPS 180-1 section 4: a single 1 bit, then zeros, then the 64-bit length. */
    static const uint8_t one = 0x80u;
    kernel_crypto_sha1_absorb(state, &one, 1u);

    static const uint8_t zeros[64] = {0};
    /* Pad until exactly 8 bytes remain in the block for the length field. An
     * off-by-one here still yields a 20-byte digest, so only a vector catches it. */
    while (state->pending != 56u) {
        size_t want = (state->pending < 56u) ? (56u - state->pending) : (120u - state->pending);
        kernel_crypto_sha1_absorb(state, zeros, want);
    }

    /* BIG-ENDIAN, which is the single most common porting defect in SHA-1: a
     * little-endian length field produces a digest that is wrong only in its
     * final block, so every short-message vector still fails but a casual
     * round-trip test against our own code would not. */
    uint8_t tail[8];
    for (unsigned i = 0u; i < 8u; i++) {
        tail[i] = (uint8_t)((bits >> (56u - 8u * i)) & 0xFFu);
    }
    kernel_crypto_sha1_absorb(state, tail, sizeof tail);

    /* The digest is the chaining value, also big-endian. */
    for (unsigned i = 0u; i < 5u; i++) {
        digest[i * 4u + 0u] = (uint8_t)((state->chain[i] >> 24) & 0xFFu);
        digest[i * 4u + 1u] = (uint8_t)((state->chain[i] >> 16) & 0xFFu);
        digest[i * 4u + 2u] = (uint8_t)((state->chain[i] >> 8) & 0xFFu);
        digest[i * 4u + 3u] = (uint8_t)(state->chain[i] & 0xFFu);
    }
}

void kernel_crypto_sha1(const void *data, size_t length, uint8_t digest[20])
{
    kernel_sha1_state state;
    kernel_crypto_sha1_reset(&state);
    kernel_crypto_sha1_absorb(&state, data, length);
    kernel_crypto_sha1_squeeze(&state, digest);
}

/* ======================================================================
 * HMAC-SHA1, from RFC 2104, and the Xbox variant of it.
 * ====================================================================== */

#define HMAC_BLOCK 64u

/*
 * One core for both policies, because the ONLY difference between RFC 2104 and the
 * Xbox export is how a key longer than the block is reduced, and writing it twice
 * would let the two drift. Two message segments because the Xbox export takes two
 * and RFC 2104 callers pass NULL for the second: HMAC over a concatenation is
 * exactly HMAC over the segments fed in order, so this costs nothing.
 */
static void hmac_sha1_core(const uint8_t *key, size_t key_length, bool hash_long_key,
                           const void *first, size_t first_length, const void *second,
                           size_t second_length, uint8_t mac[20])
{
    uint8_t padded[HMAC_BLOCK];
    memset(padded, 0, sizeof padded);

    if (key_length > HMAC_BLOCK) {
        if (hash_long_key) {
            /* RFC 2104: a key longer than the block is HASHED to 20 bytes first. */
            uint8_t reduced[20];
            kernel_crypto_sha1(key, key_length, reduced);
            memcpy(padded, reduced, sizeof reduced);
        } else {
            /* The Xbox policy: TRUNCATE to the block. See kernel_crypto.h for why
             * this is modelled separately rather than assumed to be RFC 2104. */
            memcpy(padded, key, HMAC_BLOCK);
        }
    } else if (key_length != 0u) {
        memcpy(padded, key, key_length);
    }

    uint8_t inner_pad[HMAC_BLOCK];
    uint8_t outer_pad[HMAC_BLOCK];
    for (unsigned i = 0u; i < HMAC_BLOCK; i++) {
        /* RFC 2104: ipad is 0x36 and opad is 0x5C. Swapping them is a mutation
         * target because the result is still a plausible 20-byte MAC. */
        inner_pad[i] = (uint8_t)(padded[i] ^ 0x36u);
        outer_pad[i] = (uint8_t)(padded[i] ^ 0x5Cu);
    }

    uint8_t inner[20];
    kernel_sha1_state state;
    kernel_crypto_sha1_reset(&state);
    kernel_crypto_sha1_absorb(&state, inner_pad, sizeof inner_pad);
    kernel_crypto_sha1_absorb(&state, first, first_length);
    kernel_crypto_sha1_absorb(&state, second, second_length);
    kernel_crypto_sha1_squeeze(&state, inner);

    kernel_crypto_sha1_reset(&state);
    kernel_crypto_sha1_absorb(&state, outer_pad, sizeof outer_pad);
    kernel_crypto_sha1_absorb(&state, inner, sizeof inner);
    kernel_crypto_sha1_squeeze(&state, mac);
}

void kernel_crypto_hmac_sha1(const void *key, size_t key_length, const void *message,
                             size_t message_length, uint8_t mac[20])
{
    hmac_sha1_core((const uint8_t *)key, key_length, true, message, message_length, NULL, 0u,
                   mac);
}

void kernel_crypto_xc_hmac(const void *key, size_t key_length, const void *first,
                           size_t first_length, const void *second, size_t second_length,
                           uint8_t mac[20])
{
    hmac_sha1_core((const uint8_t *)key, key_length, false, first, first_length, second,
                   second_length, mac);
}

/* ======================================================================
 * DES key parity, from FIPS 46-3 / ANSI X3.92.
 * ====================================================================== */

bool kernel_crypto_des_key_parity(void *key, size_t length)
{
    if (!key && length != 0u) {
        return false;
    }
    uint8_t *bytes = (uint8_t *)key;
    bool changed = false;
    for (size_t i = 0u; i < length; i++) {
        /* The low bit of each byte is the parity bit: it is set so the byte has
         * ODD population count. The seven high bits are key material and are
         * never touched. */
        unsigned ones = 0u;
        for (unsigned bit = 1u; bit < 8u; bit++) {
            if ((bytes[i] & (uint8_t)(1u << bit)) != 0u) {
                ones++;
            }
        }
        const uint8_t wanted = (uint8_t)((ones % 2u == 0u) ? 1u : 0u);
        const uint8_t adjusted = (uint8_t)((bytes[i] & 0xFEu) | wanted);
        if (adjusted != bytes[i]) {
            changed = true;
            bytes[i] = adjusted;
        }
    }
    return changed;
}

/* ======================================================================
 * The guest boundary.
 *
 * THE SHA CONTEXT LIVES IN THE GUEST'S OWN 116 BYTES, in a layout of OUR
 * choosing. We do not know Microsoft's layout and do not guess it: see
 * kernel_crypto.h for the three independent measurements that put the guest's
 * reservation at 116 bytes, and for why a host-side table keyed by the context
 * pointer was rejected (the guest keeps contexts embedded in heap objects, three
 * to a structure, so any fixed-capacity host table would silently overflow).
 *
 * Every field below is 32-bit and written through the guest accessors, so the
 * serialised form does not depend on the host's word size or padding.
 * ====================================================================== */

/* 'TSHA' little-endian. A context the guest zeroed, copied, or never initialised
 * fails this check, and that is REPORTED rather than hashed as if it were valid. */
#define SHA_CTX_MAGIC 0x41485354u

#define SHA_CTX_OFF_MAGIC 0u
#define SHA_CTX_OFF_CHAIN 4u /* 5 words */
#define SHA_CTX_OFF_BITS_LO 24u
#define SHA_CTX_OFF_BITS_HI 28u
#define SHA_CTX_OFF_PENDING 32u
#define SHA_CTX_OFF_BLOCK 36u /* 64 bytes */

/* 36 + 64. Comfortably inside the 116 bytes the guest reserves, which a
 * _Static_assert pins so the two cannot drift apart. */
#define SHA_CTX_BYTES 100u

/*
 * The guest's own reservation, MEASURED three ways (kernel_crypto.h). Asserted
 * rather than commented: shrinking our layout is harmless, but growing past this
 * would overwrite the guest's next field and the damage would surface elsewhere.
 */
#define GUEST_SHA_CTX_BYTES 116u
_Static_assert(SHA_CTX_BYTES <= GUEST_SHA_CTX_BYTES,
               "our SHA context must fit in the 116 bytes the guest reserves");

/* The digest every one of these exports produces. */
#define SHA_DIGEST_BYTES 20u

static unsigned sha_init_calls;
static unsigned sha_update_calls;
static unsigned sha_final_calls;
static unsigned hmac_calls;
static unsigned des_parity_calls;
static unsigned stale_context_count;
static unsigned hmac_oversize_key_count;
static unsigned long long sha_bytes_absorbed;

static bool sha_ctx_store(kernel_guest_ptr context, const kernel_sha1_state *state)
{
    if (!kernel_guest_at(context, SHA_CTX_BYTES)) {
        return false;
    }
    if (!kernel_guest_write_u32(context + SHA_CTX_OFF_MAGIC, SHA_CTX_MAGIC) ||
        !kernel_guest_write_u32(context + SHA_CTX_OFF_BITS_LO,
                                (uint32_t)(state->bits & 0xFFFFFFFFu)) ||
        !kernel_guest_write_u32(context + SHA_CTX_OFF_BITS_HI,
                                (uint32_t)(state->bits >> 32)) ||
        !kernel_guest_write_u32(context + SHA_CTX_OFF_PENDING, (uint32_t)state->pending)) {
        return false;
    }
    for (unsigned i = 0u; i < 5u; i++) {
        if (!kernel_guest_write_u32(context + SHA_CTX_OFF_CHAIN + i * 4u, state->chain[i])) {
            return false;
        }
    }
    for (unsigned i = 0u; i < 64u; i++) {
        if (!kernel_guest_write_u8(context + SHA_CTX_OFF_BLOCK + i, state->block[i])) {
            return false;
        }
    }
    return true;
}

static bool sha_ctx_load(kernel_guest_ptr context, kernel_sha1_state *state)
{
    if (!kernel_guest_at(context, SHA_CTX_BYTES)) {
        return false;
    }
    uint32_t magic = 0u;
    if (!kernel_guest_read_u32(context + SHA_CTX_OFF_MAGIC, &magic)) {
        return false;
    }
    if (magic != SHA_CTX_MAGIC) {
        return false;
    }
    uint32_t low = 0u;
    uint32_t high = 0u;
    uint32_t pending = 0u;
    if (!kernel_guest_read_u32(context + SHA_CTX_OFF_BITS_LO, &low) ||
        !kernel_guest_read_u32(context + SHA_CTX_OFF_BITS_HI, &high) ||
        !kernel_guest_read_u32(context + SHA_CTX_OFF_PENDING, &pending)) {
        return false;
    }
    /* A pending count at or above the block size means the bytes are not ours,
     * whatever the magic says. Clamping it instead would hash 64 bytes of somebody
     * else's memory. */
    if (pending >= 64u) {
        return false;
    }
    state->bits = ((uint64_t)high << 32) | (uint64_t)low;
    state->pending = (size_t)pending;
    for (unsigned i = 0u; i < 5u; i++) {
        if (!kernel_guest_read_u32(context + SHA_CTX_OFF_CHAIN + i * 4u, &state->chain[i])) {
            return false;
        }
    }
    for (unsigned i = 0u; i < 64u; i++) {
        if (!kernel_guest_read_u8(context + SHA_CTX_OFF_BLOCK + i, &state->block[i])) {
            return false;
        }
    }
    return true;
}

/*
 * ARITY-OK(335): ONE stack argument, the context pointer. The measured table has NO
 * row for this ordinal -- all 17 sites reach it through the import jump stub at
 * 0x00384876 (`jmp [0x475830]`), which callsites.py cannot bracket, so it casts no
 * arity vote at all. Counted instead from the decompiler's own reconstruction at
 * every site: 15 reconstructed calls, UNANIMOUSLY one argument, zero sites with any
 * other count. Site table in kernel_crypto.h.
 */
static uint32_t hle_xc_sha_init(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: XcSHAInit called with no argument frame -- the call "
                         "boundary did not supply one\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t ctx = 0u;
    if (!kernel_frame_arg(frame, 0u, &ctx)) {
        kernel_hle_log()("kernel: XcSHAInit could not read its context argument from the "
                         "guest stack\n");
        return STATUS_INVALID_PARAMETER;
    }
    kernel_sha1_state state;
    kernel_crypto_sha1_reset(&state);
    if (!sha_ctx_store(ctx, &state)) {
        kernel_hle_log()("kernel: XcSHAInit cannot write the %u-byte context at %#x\n",
                         (unsigned)SHA_CTX_BYTES, (unsigned)ctx);
        return STATUS_INVALID_PARAMETER;
    }
    sha_init_calls++;
    return STATUS_SUCCESS;
}

/*
 * ARITY-OK(336): THREE stack arguments, (context, data, length). No measured row,
 * for the same stub reason as 335 -- the stub is at 0x00384870 (`jmp [0x47582C]`),
 * and there is a SECOND stub for this same ordinal at 0x0037E656, which is itself
 * evidence the measurement cannot see these sites. 23 reconstructed calls,
 * UNANIMOUSLY three arguments. The order is pinned independently of the count at
 * 0x00380684, where argument 2 is the literal 0x44 and argument 1 is the address of
 * the 0x44-byte buffer just read from the file: a length cannot be the buffer and a
 * stack address cannot be the length.
 */
static uint32_t hle_xc_sha_update(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: XcSHAUpdate called with no argument frame -- the call "
                         "boundary did not supply one\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t args[3];
    for (unsigned i = 0u; i < 3u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: XcSHAUpdate could not read argument %u from the "
                             "guest stack\n",
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    const kernel_guest_ptr ctx = args[0];
    const kernel_guest_ptr data = args[1];
    const uint32_t length = args[2];

    kernel_sha1_state state;
    if (!sha_ctx_load(ctx, &state)) {
        /* NOT recovered by starting a fresh hash, which is the tempting thing to do:
         * the guest would get a digest over a SUFFIX of its message and compare it
         * against a stored one, and the mismatch would look like a corrupt save
         * rather than like our bug. */
        stale_context_count++;
        kernel_hle_log()("kernel: XcSHAUpdate given a context at %#x this host did not "
                         "initialise -- refusing rather than hashing from an unknown "
                         "state\n",
                         (unsigned)ctx);
        return STATUS_INVALID_PARAMETER;
    }

    if (length != 0u) {
        const void *bytes = kernel_guest_at(data, (size_t)length);
        if (!bytes) {
            kernel_hle_log()("kernel: XcSHAUpdate cannot read %u bytes at %#x\n",
                             (unsigned)length, (unsigned)data);
            return STATUS_INVALID_PARAMETER;
        }
        kernel_crypto_sha1_absorb(&state, bytes, (size_t)length);
        sha_bytes_absorbed += (unsigned long long)length;
    }

    if (!sha_ctx_store(ctx, &state)) {
        kernel_hle_log()("kernel: XcSHAUpdate cannot write back the context at %#x\n",
                         (unsigned)ctx);
        return STATUS_INVALID_PARAMETER;
    }
    sha_update_calls++;
    return STATUS_SUCCESS;
}

/*
 * ARITY-OK(337): TWO stack arguments, (context, digest out). No measured row, stub
 * at 0x0038487C (`jmp [0x475834]`). 16 reconstructed calls, UNANIMOUSLY two
 * arguments. The order is pinned at 0x00439897, where argument 0 is the same
 * `local_8c` the matching XcSHAInit was given and argument 1 is a FIVE-DWORD local
 * (`undefined4 local_18[5]`) that is immediately copied out: 5 dwords is 20 bytes,
 * the SHA-1 digest length, and not a context.
 */
static uint32_t hle_xc_sha_final(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: XcSHAFinal called with no argument frame -- the call "
                         "boundary did not supply one\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t args[2];
    for (unsigned i = 0u; i < 2u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: XcSHAFinal could not read argument %u from the "
                             "guest stack\n",
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    const kernel_guest_ptr ctx = args[0];
    const kernel_guest_ptr digest_out = args[1];

    kernel_sha1_state state;
    if (!sha_ctx_load(ctx, &state)) {
        stale_context_count++;
        kernel_hle_log()("kernel: XcSHAFinal given a context at %#x this host did not "
                         "initialise -- refusing rather than returning a digest of "
                         "nothing\n",
                         (unsigned)ctx);
        return STATUS_INVALID_PARAMETER;
    }

    uint8_t *out = (uint8_t *)kernel_guest_at(digest_out, SHA_DIGEST_BYTES);
    if (!out) {
        kernel_hle_log()("kernel: XcSHAFinal cannot write %u digest bytes at %#x\n",
                         (unsigned)SHA_DIGEST_BYTES, (unsigned)digest_out);
        return STATUS_INVALID_PARAMETER;
    }
    kernel_crypto_sha1_squeeze(&state, out);

    /* The context is spent. Written back INVALIDATED rather than left holding the
     * pre-padding state, so a second XcSHAFinal on the same context is REPORTED
     * instead of quietly producing a different digest. */
    if (!kernel_guest_write_u32(ctx + SHA_CTX_OFF_MAGIC, 0u)) {
        kernel_hle_log()("kernel: XcSHAFinal could not invalidate the context at %#x\n",
                         (unsigned)ctx);
        return STATUS_INVALID_PARAMETER;
    }
    sha_final_calls++;
    return STATUS_SUCCESS;
}

/*
 * ARITY-OK(340): SEVEN stack arguments,
 * (key, key length, data1, length1, data2, length2, mac out). No measured row, stub
 * at 0x00384882 (`jmp [0x475870]`). ALL 40 reconstructed calls take SEVEN arguments
 * -- a count that agrees with ordinal_callsites.json's 40 sites exactly, with no
 * site reporting any other number.
 *
 * SEVEN, NOT FIVE, AND THAT IS THE POINT. This is not textbook
 * HMAC(key, keylen, message, msglen, out): the Xbox export takes TWO message
 * segments. 33 of the 40 sites pass a literal 0 for BOTH segment-2 arguments, which
 * is exactly what would make it look like a five-argument function to anyone who
 * read only those; the remaining 7 pass a real pointer and a real length, five of
 * them the literal 0x10. Argument order comes from the literals rather than from
 * assumption: at 0x00422713 the call is
 * (XboxHDKey, 0x10, buffer, 500, 0, 0, local_18), and 500 is the length of the
 * region whose stored MAC sits at buffer+500.
 */
static uint32_t hle_xc_hmac(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: XcHMAC called with no argument frame -- the call "
                         "boundary did not supply one\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t args[7];
    for (unsigned i = 0u; i < 7u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: XcHMAC could not read argument %u from the guest "
                             "stack\n",
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    const kernel_guest_ptr key_ptr = args[0];
    const uint32_t key_length = args[1];
    const kernel_guest_ptr first_ptr = args[2];
    const uint32_t first_length = args[3];
    const kernel_guest_ptr second_ptr = args[4];
    const uint32_t second_length = args[5];
    const kernel_guest_ptr mac_out = args[6];

    const void *key = NULL;
    if (key_length != 0u) {
        key = kernel_guest_at(key_ptr, (size_t)key_length);
        if (!key) {
            kernel_hle_log()("kernel: XcHMAC cannot read a %u-byte key at %#x\n",
                             (unsigned)key_length, (unsigned)key_ptr);
            return STATUS_INVALID_PARAMETER;
        }
    }

    /* SEGMENT 2 IS OPTIONAL, and that is measured rather than assumed: 33 of the 40
     * sites pass a literal 0 for both of its arguments. A handler that translated a
     * zero pointer unconditionally would refuse the common case. */
    const void *first = NULL;
    if (first_length != 0u) {
        first = kernel_guest_at(first_ptr, (size_t)first_length);
        if (!first) {
            kernel_hle_log()("kernel: XcHMAC cannot read %u bytes of segment 1 at %#x\n",
                             (unsigned)first_length, (unsigned)first_ptr);
            return STATUS_INVALID_PARAMETER;
        }
    }
    const void *second = NULL;
    if (second_length != 0u) {
        second = kernel_guest_at(second_ptr, (size_t)second_length);
        if (!second) {
            kernel_hle_log()("kernel: XcHMAC cannot read %u bytes of segment 2 at %#x\n",
                             (unsigned)second_length, (unsigned)second_ptr);
            return STATUS_INVALID_PARAMETER;
        }
    }

    uint8_t *out = (uint8_t *)kernel_guest_at(mac_out, SHA_DIGEST_BYTES);
    if (!out) {
        kernel_hle_log()("kernel: XcHMAC cannot write %u MAC bytes at %#x\n",
                         (unsigned)SHA_DIGEST_BYTES, (unsigned)mac_out);
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * THE ONE PLACE OUR ANSWER COULD DIFFER FROM HARDWARE, counted so nobody has to
     * take it on trust. No measured site passes a key longer than the 64-byte block
     * -- 33 pass the literal 0x10, one 0xc, six pass a runtime value -- so the
     * RFC-2104-versus-truncate question is UNREACHABLE at every site we can read. It
     * is still counted, because a runtime key length could in principle exceed the
     * block, and then this choice would start to matter.
     */
    if (key_length > HMAC_BLOCK) {
        hmac_oversize_key_count++;
        kernel_hle_log()("kernel: XcHMAC given a %u-byte key, longer than the 64-byte "
                         "block -- no measured call site does this, so the reduction "
                         "policy is UNVERIFIED against hardware. See kernel_crypto.h\n",
                         (unsigned)key_length);
    }

    kernel_crypto_xc_hmac(key, (size_t)key_length, first, (size_t)first_length, second,
                          (size_t)second_length, out);
    hmac_calls++;
    return STATUS_SUCCESS;
}

/*
 * ARITY-OK(346): TWO stack arguments, (key, length). No measured row, stub at
 * 0x003D1898. 13 reconstructed calls, UNANIMOUSLY two arguments, a count that
 * agrees with ordinal_callsites.json's 13 sites exactly. The order is pinned by the
 * literal: TWELVE of the 13 sites pass 0x18 as argument 1, and 0x18 is 24 bytes,
 * a THREE-key DES bundle. A stack address cannot be 0x18 at twelve sites.
 *
 * WHAT 24 BYTES TELLS US, AND WHAT IT DOES NOT. It says the caller is building a
 * 3DES key bundle, which 0x0041524E confirms by handing the same buffer straight to
 * XcKeyTable. It does NOT tell us the parity convention; that comes from FIPS 46-3
 * and is pinned by published weak keys. See kernel_crypto.h.
 */
static uint32_t hle_xc_des_key_parity(void *context)
{
    if (!context) {
        kernel_hle_log()("kernel: XcDESKeyParity called with no argument frame -- the "
                         "call boundary did not supply one\n");
        return STATUS_INVALID_PARAMETER;
    }
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    uint32_t args[2];
    for (unsigned i = 0u; i < 2u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: XcDESKeyParity could not read argument %u from the "
                             "guest stack\n",
                             i);
            return STATUS_INVALID_PARAMETER;
        }
    }
    const kernel_guest_ptr key_ptr = args[0];
    const uint32_t length = args[1];

    if (length == 0u) {
        /* Not an error on hardware -- adjusting nothing succeeds -- but no measured
         * site does it, so it is announced rather than passed over in silence. */
        kernel_hle_log()("kernel: XcDESKeyParity asked to adjust 0 bytes at %#x\n",
                         (unsigned)key_ptr);
        des_parity_calls++;
        return STATUS_SUCCESS;
    }

    void *key = kernel_guest_at(key_ptr, (size_t)length);
    if (!key) {
        kernel_hle_log()("kernel: XcDESKeyParity cannot write %u bytes at %#x\n",
                         (unsigned)length, (unsigned)key_ptr);
        return STATUS_INVALID_PARAMETER;
    }
    (void)kernel_crypto_des_key_parity(key, (size_t)length);
    des_parity_calls++;
    return STATUS_SUCCESS;
}

unsigned kernel_crypto_register(void)
{
    static const struct {
        unsigned ordinal;
        kernel_fn handler;
    } bindings[] = {
        {ORD_XcSHAInit, hle_xc_sha_init},
        {ORD_XcSHAUpdate, hle_xc_sha_update},
        {ORD_XcSHAFinal, hle_xc_sha_final},
        {ORD_XcRC4Key, hle_xc_rc4_key},
        {ORD_XcRC4Crypt, hle_xc_rc4_crypt},
        {ORD_XcHMAC, hle_xc_hmac},
        {ORD_XcDESKeyParity, hle_xc_des_key_parity},
    };

    unsigned bound = 0u;
    for (size_t i = 0u; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        if (kernel_hle_register(bindings[i].ordinal, bindings[i].handler)) {
            bound++;
        }
    }
    return bound;
}

void kernel_crypto_reset(void)
{
    sha_init_calls = 0u;
    sha_update_calls = 0u;
    sha_final_calls = 0u;
    hmac_calls = 0u;
    des_parity_calls = 0u;
    stale_context_count = 0u;
    hmac_oversize_key_count = 0u;
    sha_bytes_absorbed = 0u;
}

unsigned kernel_crypto_sha_init_count(void)
{
    return sha_init_calls;
}

unsigned kernel_crypto_sha_update_count(void)
{
    return sha_update_calls;
}

unsigned kernel_crypto_sha_final_count(void)
{
    return sha_final_calls;
}

unsigned kernel_crypto_hmac_count(void)
{
    return hmac_calls;
}

unsigned kernel_crypto_des_parity_count(void)
{
    return des_parity_calls;
}

unsigned kernel_crypto_stale_context_count(void)
{
    return stale_context_count;
}

unsigned kernel_crypto_hmac_oversize_key_count(void)
{
    return hmac_oversize_key_count;
}

unsigned long long kernel_crypto_sha_bytes_absorbed(void)
{
    return sha_bytes_absorbed;
}

size_t kernel_crypto_guest_context_bytes(void)
{
    return (size_t)SHA_CTX_BYTES;
}
