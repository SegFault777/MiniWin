#ifndef GCM_H
#define GCM_H
#include "io.h"
#include "aes.h"

typedef unsigned long long u64;

/* ============================================================
 * gcm.h -- AES-GCM, per NIST SP 800-38D. The AEAD (Authenticated
 * Encryption with Associated Data) cipher this client actually needs:
 * discovered during development that the servers/gateways worth
 * talking to on the real internet have largely retired CBC-mode cipher
 * suites (BEAST/Lucky13-era hardening) in favor of AEAD ones, so GCM
 * isn't a nice-to-have alternative to kernel/aes.h's CBC mode -- for
 * this client's one supported cipher suite
 * (TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256), it's the only mode that
 * matters. GCM also folds authentication into the same pass as
 * encryption (the output IS the auth tag; no separate HMAC step,
 * unlike CBC's MAC-then-encrypt), which is part of why it replaced CBC
 * industry-wide rather than just supplementing it.
 *
 * Two pieces: CTR-mode encryption (built directly on aes.h's block
 * primitive -- encrypt a counter, XOR with plaintext, increment,
 * repeat) and GHASH, a MAC built from multiplication in GF(2^128).
 *
 * pre-21 rewrite: GHASH used to be the spec appendix's bit-serial
 * multiply (128 shift-and-xor steps PER 16-byte block) fed from a
 * 16KB stack scratch buffer. Two problems: a 16KB record cost on the
 * order of a hundred thousand of those steps per block-batch -- painful
 * once pages are 100KB+ -- and the scratch buffer alone was a third of
 * the old stack's entire budget. Now it is the standard 4-bit table
 * method (Shoup): precompute the 16 multiples of H by every 4-bit
 * value once per call, then each block is 32 table lookups. And it
 * streams: AAD, ciphertext and the length block are absorbed
 * piece by piece into a 16-byte accumulator, so nothing needs
 * assembling in a buffer first. It also takes either AES key size
 * (the context knows its own round count).
 * ============================================================ */

#define GCM_BLOCK_SIZE 16
#define GCM_TAG_SIZE   16
#define GCM_IV_SIZE    12   /* TLS's GCM record nonce: a 4-byte fixed
                            * salt (from key derivation) followed by an
                            * 8-byte explicit per-record value -- see
                            * kernel/tls.h for how those two pieces get
                            * assembled into this 12-byte whole */

/* The protocol's own per-record ceiling for what goes through one
 * GCM call: 16384 bytes of plaintext (RFC 5246/8446) plus AEAD
 * expansion. gcm itself no longer needs a buffer this size (it
 * streams) -- the constant remains for tls.h's record sizing. */
#define GCM_MAX_PAYLOAD 16416

static inline void gcm_xor_block(u8 out[16], const u8 a[16], const u8 b[16]) {
    for (int i = 0; i < 16; i++) out[i] = (u8)(a[i] ^ b[i]);
}

/* GF(2^128) multiplication, per SP 800-38D Algorithm 1: processes X's
 * 128 bits MSB-first, conditionally XORing a running copy of Y (which
 * itself gets right-shifted each round, with the reduction polynomial
 * R=0xE1000...0 XORed in whenever a 1 bit would otherwise fall off the
 * bottom). This one function, called once per 16-byte block of
 * associated data and ciphertext, is GHASH's entire mathematical
 * content -- everything else about GHASH is just chaining these
 * multiplies block by block. */
/* ---- GHASH via 4-bit tables -----------------------------------
 * The GHASH key H (= AES_K(0^128)) is a fixed element of GF(2^128).
 * Multiplying an arbitrary block X by H bit-serially is slow; but X
 * can be sliced into 32 nibbles and H*nibble precomputed for all 16
 * nibble values (two u64 halves each => 256 bytes). Then X*H is: walk
 * the nibbles from the last byte to the first, and for each one
 * shift the running result right by 4 bits (folding the 4 bits that
 * fall off back in via the reduction table `last4`) and XOR in the
 * table entry. Bit order note: GCM numbers bits "backwards" (bit 0 is
 * the MOST significant bit of byte 0), which is why the reduction
 * polynomial shows up as 0xE1 in the top byte and shifts go right. */
typedef struct {
    u64 hl[16];   /* low  64 bits of nibble*H */
    u64 hh[16];   /* high 64 bits of nibble*H */
} gcm_table_t;

static inline u64 gcm_get64_be(const u8 *p) {
    return ((u64)p[0] << 56) | ((u64)p[1] << 48) | ((u64)p[2] << 40) | ((u64)p[3] << 32) |
           ((u64)p[4] << 24) | ((u64)p[5] << 16) | ((u64)p[6] << 8)  |  (u64)p[7];
}
static inline void gcm_put64_be(u8 *p, u64 v) {
    for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (56 - 8 * i));
}

static inline void gcm_build_table(gcm_table_t *t, const u8 h[16]) {
    u64 vh = gcm_get64_be(h);
    u64 vl = gcm_get64_be(h + 8);

    t->hl[0] = 0; t->hh[0] = 0;
    t->hl[8] = vl; t->hh[8] = vh;          /* index 8 = the single bit "1000" = H itself */
    for (int i = 4; i > 0; i >>= 1) {      /* 4, 2, 1 = H shifted right (i.e. multiplied by x) once each */
        u64 carry = (vl & 1) ? 0xe100000000000000ULL : 0;
        vl = (vh << 63) | (vl >> 1);
        vh = (vh >> 1) ^ carry;
        t->hl[i] = vl; t->hh[i] = vh;
    }
    for (int i = 2; i < 16; i <<= 1) {     /* every other entry is an XOR of power-of-two entries */
        for (int j = 1; j < i; j++) {
            t->hh[i + j] = t->hh[i] ^ t->hh[j];
            t->hl[i + j] = t->hl[i] ^ t->hl[j];
        }
    }
}

/* y = y * H, in place. */
static inline void gcm_mult_h(const gcm_table_t *t, u8 y[16]) {
    static const u64 last4[16] = {
        0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
        0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0 };
    u32 lo = y[15] & 0xf;
    u64 zh = t->hh[lo], zl = t->hl[lo];
    for (int i = 15; i >= 0; i--) {
        lo = y[i] & 0xf;
        u32 hi = (y[i] >> 4) & 0xf;
        if (i != 15) {
            u32 rem = (u32)(zl & 0xf);
            zl = (zh << 60) | (zl >> 4);
            zh = zh >> 4;
            zh ^= last4[rem] << 48;
            zh ^= t->hh[lo];
            zl ^= t->hl[lo];
        }
        u32 rem = (u32)(zl & 0xf);
        zl = (zh << 60) | (zl >> 4);
        zh = zh >> 4;
        zh ^= last4[rem] << 48;
        zh ^= t->hh[hi];
        zl ^= t->hl[hi];
    }
    gcm_put64_be(y, zh);
    gcm_put64_be(y + 8, zl);
}

/* Folds `len` bytes into the GHASH accumulator y, zero-padding the
 * final partial block (which is exactly what GCM specifies for AAD and
 * ciphertext each: pad each to a 16-byte boundary independently). */
static inline void gcm_absorb(const gcm_table_t *t, u8 y[16], const u8 *data, u32 len) {
    while (len >= 16) {
        for (int i = 0; i < 16; i++) y[i] ^= data[i];
        gcm_mult_h(t, y);
        data += 16; len -= 16;
    }
    if (len > 0) {
        for (u32 i = 0; i < len; i++) y[i] ^= data[i];   /* the rest of the block is an implicit 0-pad */
        gcm_mult_h(t, y);
    }
}

/* The final GHASH block: [len(AAD) in bits]_64 || [len(C) in bits]_64. */
static inline void gcm_absorb_lengths(const gcm_table_t *t, u8 y[16], u32 aad_len, u32 ct_len) {
    u64 aad_bits = (u64)aad_len * 8, ct_bits = (u64)ct_len * 8;
    u8 blk[16];
    gcm_put64_be(blk, aad_bits);
    gcm_put64_be(blk + 8, ct_bits);
    for (int i = 0; i < 16; i++) y[i] ^= blk[i];
    gcm_mult_h(t, y);
}

static inline void gcm_inc32(u8 counter[16]) {
    /* increments only the last 4 bytes, wrapping within them -- per
     * spec, GCM's counter deliberately does NOT carry into the fixed
     * nonce portion of the block even on overflow (not a concern here:
     * no single TLS record this client ever sends or receives is
     * remotely close to 2^32 blocks) */
    for (int i = 15; i >= 12; i--) {
        counter[i]++;
        if (counter[i] != 0) break;
    }
}

/* CTR-mode keystream generation/application: XORs `data` in place with
 * successive AES_encrypt(counter) blocks, incrementing counter (via
 * gcm_inc32) between blocks. Identical operation for encrypt and
 * decrypt -- CTR mode's defining, convenient property. */
static inline void gcm_ctr_apply(const aes_ctx_t *aes, u8 counter[16], u8 *data, u32 len) {
    u8 keystream[16];
    u32 off = 0;
    while (off < len) {
        for (int i = 0; i < 16; i++) keystream[i] = counter[i];
        aes128_encrypt_block(aes, keystream);
        u32 chunk = (len - off < 16) ? (len - off) : 16;
        for (u32 i = 0; i < chunk; i++) data[off + i] = (u8)(data[off + i] ^ keystream[i]);
        gcm_inc32(counter);
        off += chunk;
    }
}

/* Computes the GCM tag over (aad, ciphertext) for a 96-bit IV:
 *   tag = GHASH_H(aad, ciphertext, lengths) XOR AES_K(J0),  J0 = IV || 0x00000001 */
static inline void gcm_compute_tag(const aes_ctx_t *aes, const u8 iv[GCM_IV_SIZE],
                                   const u8 *aad, u32 aad_len,
                                   const u8 *ct, u32 ct_len,
                                   u8 tag_out[GCM_TAG_SIZE]) {
    u8 h[16] = {0};
    aes128_encrypt_block(aes, h);           /* H = E(K, 0^128) */

    gcm_table_t table;
    gcm_build_table(&table, h);

    u8 y[16] = {0};
    gcm_absorb(&table, y, aad, aad_len);
    gcm_absorb(&table, y, ct, ct_len);
    gcm_absorb_lengths(&table, y, aad_len, ct_len);

    u8 j0[16];
    for (int i = 0; i < 12; i++) j0[i] = iv[i];
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    aes128_encrypt_block(aes, j0);          /* E(K, J0) */
    gcm_xor_block(tag_out, y, j0);
}

/* Encrypts `plaintext` (len bytes, in place) and produces the 16-byte
 * authentication tag, per SP 800-38D's algorithm for a 96-bit IV (the
 * only IV length TLS's GCM cipher suites use). `aad` is the additional
 * authenticated data (TLS's record header: sequence number, content
 * type, version, length) -- authenticated but never encrypted. */
static inline void gcm_encrypt(const aes_ctx_t *aes, const u8 iv[GCM_IV_SIZE],
                                const u8 *aad, u32 aad_len,
                                u8 *plaintext, u32 pt_len,
                                u8 tag_out[GCM_TAG_SIZE]) {
    u8 counter[16];
    for (int i = 0; i < 12; i++) counter[i] = iv[i];
    counter[12] = 0; counter[13] = 0; counter[14] = 0; counter[15] = 1;
    gcm_inc32(counter);                                   /* first data counter is J0+1 */
    gcm_ctr_apply(aes, counter, plaintext, pt_len);       /* plaintext -> ciphertext, in place */
    gcm_compute_tag(aes, iv, aad, aad_len, plaintext, pt_len, tag_out);
}

/* Decrypts `ciphertext` (in place) after independently recomputing the
 * tag to compare against `tag_in`. Returns 1 if the tag matches (data
 * is authentic and now decrypted in place), 0 if it doesn't -- and in
 * that case the ciphertext is left UNTOUCHED (the tag is checked over
 * the ciphertext BEFORE any decryption, so a forged record never even
 * becomes plaintext in the buffer). GCM's whole security argument
 * rests on that check coming first, so kernel/tls.h treats a failed
 * gcm_decrypt() as a corrupted/tampered record: tear the connection
 * down, don't salvage anything. */
static inline int gcm_decrypt(const aes_ctx_t *aes, const u8 iv[GCM_IV_SIZE],
                               const u8 *aad, u32 aad_len,
                               u8 *ciphertext, u32 ct_len,
                               const u8 tag_in[GCM_TAG_SIZE]) {
    u8 expected[16];
    gcm_compute_tag(aes, iv, aad, aad_len, ciphertext, ct_len, expected);

    u8 diff = 0;
    for (int i = 0; i < 16; i++) diff |= (u8)(expected[i] ^ tag_in[i]);
    if (diff != 0) return 0;

    u8 counter[16];
    for (int i = 0; i < 12; i++) counter[i] = iv[i];
    counter[12] = 0; counter[13] = 0; counter[14] = 0; counter[15] = 1;
    gcm_inc32(counter);
    gcm_ctr_apply(aes, counter, ciphertext, ct_len);
    return 1;
}

#endif
