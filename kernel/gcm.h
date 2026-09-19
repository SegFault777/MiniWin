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
 * The GHASH multiply here is the textbook bit-serial algorithm from
 * the spec's own appendix -- not the 4-bit or 8-bit lookup-table
 * speedups real implementations use, matching this project's general
 * "correct and simple over fast" stance on cryptographic primitives
 * (see aes.h's own file header for the same reasoning applied to
 * MixColumns).
 * ============================================================ */

#define GCM_BLOCK_SIZE 16
#define GCM_TAG_SIZE   16
#define GCM_IV_SIZE    12   /* TLS's GCM record nonce: a 4-byte fixed
                            * salt (from key derivation) followed by an
                            * 8-byte explicit per-record value -- see
                            * kernel/tls.h for how those two pieces get
                            * assembled into this 12-byte whole */

/* Upper bound on the AAD+ciphertext this file's GHASH scratch buffer
 * needs to hold at once. Sized for TLS's actual maximum record
 * plaintext (16384 bytes, RFC 5246's own per-record cap) plus the
 * 13-byte AAD TLS's GCM cipher suites use and block-alignment padding
 * for both -- NOT the small, comfortable-looking number an early guess
 * (2048 bytes) might suggest is "surely big enough for one HTTP
 * request or response." That guess was wrong: found the hard way, when
 * a real server's response arrived as one large record and silently
 * overflowed a 2048-byte stack buffer instead of failing loudly.
 * Sizing to the protocol's own actual maximum, not to what a typical
 * message "should" look like, is the fix. */
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
static inline void gcm_gf128_mul(u8 z[16], const u8 x[16], const u8 y[16]) {
    u8 v[16];
    for (int i = 0; i < 16; i++) { z[i] = 0; v[i] = y[i]; }

    for (int i = 0; i < 128; i++) {
        int byte_idx = i / 8, bit_idx = 7 - (i % 8);
        if (x[byte_idx] & (1 << bit_idx)) {
            gcm_xor_block(z, z, v);
        }
        int lsb_set = v[15] & 1;
        /* v >>= 1, as a 128-bit big-endian-bit-numbered shift (i.e.
         * shifting the whole 16-byte array right by one bit, MSB side
         * fed zero) */
        for (int b = 15; b > 0; b--) v[b] = (u8)((v[b] >> 1) | ((v[b-1] & 1) << 7));
        v[0] = (u8)(v[0] >> 1);
        if (lsb_set) v[0] ^= 0xE1;
    }
}

/* GHASH(H, data): folds `data` (already required to be a whole number
 * of 16-byte blocks -- callers zero-pad AAD and ciphertext out to block
 * boundaries per the spec before calling this, same as the length
 * suffix block is just more input the caller assembles) through the
 * running Y = (Y XOR block) * H recurrence. */
static inline void gcm_ghash(u8 out[16], const u8 h[16], const u8 *data, u32 len_blocks_x16) {
    u8 y[16] = {0};
    for (u32 off = 0; off < len_blocks_x16; off += 16) {
        u8 block[16];
        gcm_xor_block(block, y, data + off);
        gcm_gf128_mul(y, block, h);
    }
    for (int i = 0; i < 16; i++) out[i] = y[i];
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
static inline void gcm_ctr_apply(const aes128_ctx_t *aes, u8 counter[16], u8 *data, u32 len) {
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

/* Encrypts `plaintext` (len bytes, in place) and produces the 16-byte
 * authentication tag, per SP 800-38D's algorithm for a 96-bit IV (the
 * only IV length TLS's GCM cipher suites use, so the more general
 * "hash the IV down if it isn't 96 bits" case from the spec is
 * intentionally not implemented -- it would never be exercised here).
 * `aad` is the additional authenticated data (TLS's GCM record header:
 * sequence number, content type, version, length) -- authenticated but
 * never encrypted. */
static inline void gcm_encrypt(const aes128_ctx_t *aes, const u8 iv[GCM_IV_SIZE],
                                const u8 *aad, u32 aad_len,
                                u8 *plaintext, u32 pt_len,
                                u8 tag_out[GCM_TAG_SIZE]) {
    u8 h[16] = {0};
    aes128_encrypt_block(aes, h); /* H = E(K, 0^128) */

    u8 j0[16];
    for (int i = 0; i < 12; i++) j0[i] = iv[i];
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;

    u8 counter[16];
    for (int i = 0; i < 16; i++) counter[i] = j0[i];
    gcm_inc32(counter);
    gcm_ctr_apply(aes, counter, plaintext, pt_len); /* plaintext -> ciphertext, in place */

    /* Assemble AAD || pad || ciphertext || pad || len(AAD)_64 ||
     * len(C)_64 into a scratch buffer for GHASH. Sized generously --
     * this client's own records (an HTTP request/response) are never
     * anywhere near this large, and kernel/tls.h enforces its own
     * smaller per-record limits well before this would matter. */
    u8 ghash_buf[GCM_MAX_PAYLOAD];
    u32 pos = 0;
    for (u32 i = 0; i < aad_len; i++) ghash_buf[pos++] = aad[i];
    while (pos % 16 != 0) ghash_buf[pos++] = 0;
    for (u32 i = 0; i < pt_len; i++) ghash_buf[pos++] = plaintext[i];
    while (pos % 16 != 0) ghash_buf[pos++] = 0;
    u64 aad_bits = (u64)aad_len * 8, ct_bits = (u64)pt_len * 8;
    for (int i = 0; i < 8; i++) ghash_buf[pos++] = (u8)(aad_bits >> (56 - i * 8));
    for (int i = 0; i < 8; i++) ghash_buf[pos++] = (u8)(ct_bits >> (56 - i * 8));

    u8 s[16];
    gcm_ghash(s, h, ghash_buf, pos);

    u8 tag_mask[16];
    for (int i = 0; i < 16; i++) tag_mask[i] = j0[i];
    aes128_encrypt_block(aes, tag_mask); /* E(K, J0) */
    gcm_xor_block(tag_out, s, tag_mask);
}

/* Decrypts `ciphertext` (in place) and independently recomputes the
 * tag to compare against `tag_in`. Returns 1 if the tag matches (data
 * is authentic and now decrypted in place), 0 if it doesn't -- in
 * which case the caller MUST discard whatever partially-decrypted
 * bytes ended up in the buffer rather than ever acting on them. GCM's
 * entire security argument rests on that tag check happening before
 * any decrypted byte is trusted, so kernel/tls.h treats a failed
 * gcm_decrypt() as equivalent to a corrupted/tampered record: tear the
 * connection down, don't try to salvage partial data. */
static inline int gcm_decrypt(const aes128_ctx_t *aes, const u8 iv[GCM_IV_SIZE],
                               const u8 *aad, u32 aad_len,
                               u8 *ciphertext, u32 ct_len,
                               const u8 tag_in[GCM_TAG_SIZE]) {
    u8 h[16] = {0};
    aes128_encrypt_block(aes, h);

    u8 j0[16];
    for (int i = 0; i < 12; i++) j0[i] = iv[i];
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;

    /* Compute the expected tag from the CIPHERTEXT (before decrypting
     * -- GHASH runs over what was actually transmitted, same value the
     * sender computed it over) */
    u8 ghash_buf[GCM_MAX_PAYLOAD];
    u32 pos = 0;
    for (u32 i = 0; i < aad_len; i++) ghash_buf[pos++] = aad[i];
    while (pos % 16 != 0) ghash_buf[pos++] = 0;
    for (u32 i = 0; i < ct_len; i++) ghash_buf[pos++] = ciphertext[i];
    while (pos % 16 != 0) ghash_buf[pos++] = 0;
    u64 aad_bits = (u64)aad_len * 8, ct_bits = (u64)ct_len * 8;
    for (int i = 0; i < 8; i++) ghash_buf[pos++] = (u8)(aad_bits >> (56 - i * 8));
    for (int i = 0; i < 8; i++) ghash_buf[pos++] = (u8)(ct_bits >> (56 - i * 8));

    u8 s[16];
    gcm_ghash(s, h, ghash_buf, pos);
    u8 tag_mask[16];
    for (int i = 0; i < 16; i++) tag_mask[i] = j0[i];
    aes128_encrypt_block(aes, tag_mask);
    u8 expected_tag[16];
    gcm_xor_block(expected_tag, s, tag_mask);

    u8 diff = 0;
    for (int i = 0; i < 16; i++) diff |= (u8)(expected_tag[i] ^ tag_in[i]);
    if (diff != 0) return 0; /* tag mismatch -- do NOT decrypt, do NOT
                              * trust anything about this record */

    u8 counter[16];
    for (int i = 0; i < 16; i++) counter[i] = j0[i];
    gcm_inc32(counter);
    gcm_ctr_apply(aes, counter, ciphertext, ct_len); /* only now, after
                                                      * the tag checked
                                                      * out, turn
                                                      * ciphertext into
                                                      * plaintext */
    return 1;
}

#endif
