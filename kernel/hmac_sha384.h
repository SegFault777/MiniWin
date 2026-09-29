#ifndef HMAC_SHA384_H
#define HMAC_SHA384_H
#include "sha512.h"

/* ============================================================
 * hmac_sha384.h -- HMAC (RFC 2104) over SHA-384, the keyed hash
 * TLS 1.2's PRF is built from in the *_SHA384 cipher suites.
 * Same recipe as hmac_sha256.h; only the hash and its block size
 * differ: SHA-384 works on 128-byte blocks (SHA-256 on 64), and
 * HMAC pads/truncates the key to exactly one block.
 * ============================================================ */

#define HMAC_SHA384_BLOCK_SIZE 128

static inline void hmac_sha384(const u8 *key, u32 key_len,
                               const u8 *msg, u32 msg_len,
                               u8 out[48]) {
    u8 key_block[HMAC_SHA384_BLOCK_SIZE];
    for (u32 i = 0; i < HMAC_SHA384_BLOCK_SIZE; i++) key_block[i] = 0;
    if (key_len > HMAC_SHA384_BLOCK_SIZE) {
        sha384(key, key_len, key_block);     /* oversized keys are hashed down first (48 bytes) */
    } else {
        for (u32 i = 0; i < key_len; i++) key_block[i] = key[i];
    }

    u8 ipad[HMAC_SHA384_BLOCK_SIZE], opad[HMAC_SHA384_BLOCK_SIZE];
    for (int i = 0; i < HMAC_SHA384_BLOCK_SIZE; i++) {
        ipad[i] = (u8)(key_block[i] ^ 0x36);
        opad[i] = (u8)(key_block[i] ^ 0x5c);
    }

    sha512_ctx_t ctx;
    u8 inner[48];
    sha384_init(&ctx);
    sha512_update(&ctx, ipad, HMAC_SHA384_BLOCK_SIZE);
    sha512_update(&ctx, msg, msg_len);
    sha384_final(&ctx, inner);

    sha384_init(&ctx);
    sha512_update(&ctx, opad, HMAC_SHA384_BLOCK_SIZE);
    sha512_update(&ctx, inner, 48);
    sha384_final(&ctx, out);
}

#endif
