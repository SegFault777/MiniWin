#ifndef SHA256_H
#define SHA256_H
#include "io.h"

typedef unsigned long long u64; /* io.h doesn't define this; net.h does
                                 * too, identically -- a duplicate exact
                                 * typedef is legal C, so this header
                                 * works whether or not net.h is also
                                 * included in the same translation unit. */

/* ============================================================
 * sha256.h -- SHA-256, straight from FIPS 180-4, no shortcuts. This is
 * the one hash function everything above it (HMAC, the TLS PRF, RSA
 * signature verification) leans on, so it earns the extra care: every
 * constant below is the literal spec constant, not a "close enough"
 * approximation, and the whole thing is checked against the standard's
 * own test vectors before anything gets to build on top of it.
 * ============================================================ */

typedef struct {
    u32 h[8];
    u8  buf[64];
    u32 buf_len;
    u64 total_len;   /* total message length in BYTES, tracked so the
                      * final length field (in bits) can be written
                      * without needing to know the total up front --
                      * SHA-256 is a streaming hash, fed a chunk at a
                      * time via sha256_update(). */
} sha256_ctx_t;

static const u32 sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static inline u32 sha256_rotr(u32 x, int n) { return (x >> n) | (x << (32 - n)); }
static inline u32 sha256_get32_be(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}
static inline void sha256_put32_be(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

/* Processes exactly one 64-byte block, folding it into ctx->h[]. This
 * is the compression function -- the actual "hashing" -- everything
 * else in this file is bookkeeping to feed it 64 bytes at a time. */
static inline void sha256_process_block(sha256_ctx_t *ctx, const u8 *block) {
    u32 w[64];
    for (int i = 0; i < 16; i++) w[i] = sha256_get32_be(block + i * 4);
    for (int i = 16; i < 64; i++) {
        u32 s0 = sha256_rotr(w[i-15], 7) ^ sha256_rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        u32 s1 = sha256_rotr(w[i-2], 17) ^ sha256_rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    u32 a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    u32 e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];

    for (int i = 0; i < 64; i++) {
        u32 S1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
        u32 ch = (e & f) ^ ((~e) & g);
        u32 temp1 = h + S1 + ch + sha256_k[i] + w[i];
        u32 S0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 temp2 = S0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

static inline void sha256_init(sha256_ctx_t *ctx) {
    /* the fractional parts of the square roots of the first 8 primes --
     * SHA-256's initial hash value, per spec, not a guess */
    ctx->h[0] = 0x6a09e667; ctx->h[1] = 0xbb67ae85;
    ctx->h[2] = 0x3c6ef372; ctx->h[3] = 0xa54ff53a;
    ctx->h[4] = 0x510e527f; ctx->h[5] = 0x9b05688c;
    ctx->h[6] = 0x1f83d9ab; ctx->h[7] = 0x5be0cd19;
    ctx->buf_len = 0;
    ctx->total_len = 0;
}

static inline void sha256_update(sha256_ctx_t *ctx, const u8 *data, u32 len) {
    ctx->total_len += len;
    while (len > 0) {
        u32 room = 64 - ctx->buf_len;
        u32 take = (len < room) ? len : room;
        for (u32 i = 0; i < take; i++) ctx->buf[ctx->buf_len + i] = data[i];
        ctx->buf_len += take;
        data += take;
        len -= take;
        if (ctx->buf_len == 64) {
            sha256_process_block(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

/* Finalizes the hash: appends the mandatory 0x80 byte, zero-pads out to
 * a 56-byte boundary (leaving exactly 8 bytes for the length), writes
 * the total bit-length as a big-endian 64-bit integer, processes
 * whatever block(s) that produced, and serializes h[] as the 32-byte
 * digest. Does not modify ctx for further updates -- this is a
 * one-shot "I'm done" operation, matching how every caller in this
 * codebase actually uses it (hash the whole thing, get the digest,
 * move on). */
static inline void sha256_final(sha256_ctx_t *ctx, u8 out[32]) {
    u64 bit_len = ctx->total_len * 8;
    u8 pad = 0x80;
    sha256_update(ctx, &pad, 1);

    u8 zero = 0;
    while (ctx->buf_len != 56) sha256_update(ctx, &zero, 1);

    u8 len_bytes[8];
    for (int i = 0; i < 8; i++) len_bytes[i] = (u8)(bit_len >> (56 - i * 8));
    /* Append the length directly rather than through sha256_update()'s
     * generic path -- update() would try to flush at exactly 64 bytes
     * buffered, which is exactly what we want here anyway, but writing
     * it directly makes the "this is the final block" step explicit
     * rather than incidental. */
    for (int i = 0; i < 8; i++) ctx->buf[ctx->buf_len + i] = len_bytes[i];
    ctx->buf_len += 8;
    sha256_process_block(ctx, ctx->buf);

    for (int i = 0; i < 8; i++) sha256_put32_be(out + i * 4, ctx->h[i]);
}

/* One-shot convenience wrapper -- init + update + final in one call,
 * for the common case of hashing a single contiguous buffer. */
static inline void sha256(const u8 *data, u32 len, u8 out[32]) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

#endif
