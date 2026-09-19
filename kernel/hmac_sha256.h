#ifndef HMAC_SHA256_H
#define HMAC_SHA256_H
#include "io.h"
#include "sha256.h"

/* ============================================================
 * hmac_sha256.h -- HMAC, per RFC 2104: hash(key XOR opad || hash(key
 * XOR ipad || message)). Used two ways further up this stack: as the
 * record-authentication MAC in TLS 1.2's CBC cipher suites, and as the
 * building block of the TLS PRF that derives every session key from
 * the premaster secret. Same "no shortcuts" policy as sha256.h --
 * checked against RFC 4231's own test vectors before anything above it
 * trusts it.
 * ============================================================ */

#define HMAC_SHA256_BLOCK_SIZE 64   /* SHA-256's own block size --
                                     * HMAC pads/truncates the key to
                                     * exactly this length */

static inline void hmac_sha256(const u8 *key, u32 key_len,
                                const u8 *msg, u32 msg_len,
                                u8 out[32]) {
    u8 key_block[HMAC_SHA256_BLOCK_SIZE];

    if (key_len > HMAC_SHA256_BLOCK_SIZE) {
        /* Oversized keys get hashed down to 32 bytes first -- per spec,
         * not a size optimization. */
        u8 hashed_key[32];
        sha256(key, key_len, hashed_key);
        for (u32 i = 0; i < 32; i++) key_block[i] = hashed_key[i];
        for (u32 i = 32; i < HMAC_SHA256_BLOCK_SIZE; i++) key_block[i] = 0;
    } else {
        for (u32 i = 0; i < key_len; i++) key_block[i] = key[i];
        for (u32 i = key_len; i < HMAC_SHA256_BLOCK_SIZE; i++) key_block[i] = 0;
    }

    u8 ipad[HMAC_SHA256_BLOCK_SIZE], opad[HMAC_SHA256_BLOCK_SIZE];
    for (int i = 0; i < HMAC_SHA256_BLOCK_SIZE; i++) {
        ipad[i] = (u8)(key_block[i] ^ 0x36);
        opad[i] = (u8)(key_block[i] ^ 0x5c);
    }

    /* inner = SHA256(ipad || message) */
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, HMAC_SHA256_BLOCK_SIZE);
    sha256_update(&ctx, msg, msg_len);
    u8 inner[32];
    sha256_final(&ctx, inner);

    /* result = SHA256(opad || inner) */
    sha256_init(&ctx);
    sha256_update(&ctx, opad, HMAC_SHA256_BLOCK_SIZE);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
}

#endif
