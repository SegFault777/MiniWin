#ifndef SHA512_H
#define SHA512_H
#include "io.h"

typedef unsigned long long u64; /* same redundant-but-legal typedef as sha256.h and friends */

/* ============================================================
 * sha512.h -- SHA-512 and SHA-384, per FIPS 180-4. SHA-384 is not a
 * different algorithm: it is SHA-512 with different starting values
 * (the IVs come from the square roots of the 9th-16th primes instead
 * of the 1st-8th) whose 64-byte output is simply cut off at 48 bytes.
 * So one core, two front doors.
 *
 * WHY THIS FILE EXISTS: TLS_ECDHE_*_WITH_AES_256_GCM_SHA384 (cipher
 * suites 0xC02C / 0xC030) runs its PRF and Finished hash through
 * SHA-384, and a large share of real certificate chains are signed
 * with sha384WithRSAEncryption / ecdsa-with-SHA384 (Sectigo's RSA
 * intermediates, every P-384 CA). The old client spoke SHA-256 only
 * and so failed on both -- with an "untrusted certificate" that was
 * really "I can't do the maths you used."
 *
 * Constraint reminder: no libgcc here, so no 64-bit division or
 * modulo. SHA-512 needs neither -- only add, rotate, shift, xor, and,
 * which the compiler expands into 32-bit pairs on its own.
 * ============================================================ */

#define SHA512_BLOCK_SIZE  128
#define SHA512_DIGEST_SIZE 64
#define SHA384_DIGEST_SIZE 48

typedef struct {
    u64 state[8];
    u8  block[SHA512_BLOCK_SIZE];
    u32 block_len;      /* bytes currently buffered in block[] */
    u64 total_len;      /* message length in BYTES so far (a 2^64-byte
                         * message is not a scenario this kernel has to
                         * survive, so the spec's 128-bit length field
                         * is filled from this one 64-bit counter) */
} sha512_ctx_t;

static const u64 sha512_k[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};

static inline u64 sha512_rotr(u64 x, int n) { return (x >> n) | (x << (64 - n)); }

static inline u64 sha512_get64_be(const u8 *p) {
    return ((u64)p[0] << 56) | ((u64)p[1] << 48) | ((u64)p[2] << 40) | ((u64)p[3] << 32) |
           ((u64)p[4] << 24) | ((u64)p[5] << 16) | ((u64)p[6] << 8)  |  (u64)p[7];
}
static inline void sha512_put64_be(u8 *p, u64 v) {
    for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (56 - 8 * i));
}

static inline void sha512_process_block(sha512_ctx_t *ctx, const u8 *block) {
    u64 w[80];
    for (int i = 0; i < 16; i++) w[i] = sha512_get64_be(block + i * 8);
    for (int i = 16; i < 80; i++) {
        u64 s0 = sha512_rotr(w[i-15], 1) ^ sha512_rotr(w[i-15], 8) ^ (w[i-15] >> 7);
        u64 s1 = sha512_rotr(w[i-2], 19) ^ sha512_rotr(w[i-2], 61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    u64 a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    u64 e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (int i = 0; i < 80; i++) {
        u64 S1 = sha512_rotr(e, 14) ^ sha512_rotr(e, 18) ^ sha512_rotr(e, 41);
        u64 ch = (e & f) ^ (~e & g);
        u64 t1 = h + S1 + ch + sha512_k[i] + w[i];
        u64 S0 = sha512_rotr(a, 28) ^ sha512_rotr(a, 34) ^ sha512_rotr(a, 39);
        u64 maj = (a & b) ^ (a & c) ^ (b & c);
        u64 t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static inline void sha512_init(sha512_ctx_t *ctx) {
    static const u64 iv[8] = {
        0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL };
    for (int i = 0; i < 8; i++) ctx->state[i] = iv[i];
    ctx->block_len = 0;
    ctx->total_len = 0;
}

/* SHA-384 = the same engine from a different starting point. */
static inline void sha384_init(sha512_ctx_t *ctx) {
    static const u64 iv[8] = {
        0xcbbb9d5dc1059ed8ULL,0x629a292a367cd507ULL,0x9159015a3070dd17ULL,0x152fecd8f70e5939ULL,
        0x67332667ffc00b31ULL,0x8eb44a8768581511ULL,0xdb0c2e0d64f98fa7ULL,0x47b5481dbefa4fa4ULL };
    for (int i = 0; i < 8; i++) ctx->state[i] = iv[i];
    ctx->block_len = 0;
    ctx->total_len = 0;
}

static inline void sha512_update(sha512_ctx_t *ctx, const u8 *data, u32 len) {
    ctx->total_len += len;
    while (len > 0) {
        u32 take = SHA512_BLOCK_SIZE - ctx->block_len;
        if (take > len) take = len;
        for (u32 i = 0; i < take; i++) ctx->block[ctx->block_len + i] = data[i];
        ctx->block_len += take;
        data += take;
        len -= take;
        if (ctx->block_len == SHA512_BLOCK_SIZE) {
            sha512_process_block(ctx, ctx->block);
            ctx->block_len = 0;
        }
    }
}

/* Pads (0x80, zeros, 128-bit big-endian bit length), runs the last
 * block(s), and writes the first `out_len` bytes of the state (64 for
 * SHA-512, 48 for SHA-384). The ctx is spent afterwards. */
static inline void sha512_final_n(sha512_ctx_t *ctx, u8 *out, u32 out_len) {
    u64 bits_hi = ctx->total_len >> 61;   /* the top 3 bits of (bytes * 8) */
    u64 bits_lo = ctx->total_len << 3;

    ctx->block[ctx->block_len++] = 0x80;
    if (ctx->block_len > SHA512_BLOCK_SIZE - 16) {
        while (ctx->block_len < SHA512_BLOCK_SIZE) ctx->block[ctx->block_len++] = 0;
        sha512_process_block(ctx, ctx->block);
        ctx->block_len = 0;
    }
    while (ctx->block_len < SHA512_BLOCK_SIZE - 16) ctx->block[ctx->block_len++] = 0;
    sha512_put64_be(ctx->block + SHA512_BLOCK_SIZE - 16, bits_hi);
    sha512_put64_be(ctx->block + SHA512_BLOCK_SIZE - 8, bits_lo);
    sha512_process_block(ctx, ctx->block);

    u8 full[64];
    for (int i = 0; i < 8; i++) sha512_put64_be(full + i * 8, ctx->state[i]);
    for (u32 i = 0; i < out_len; i++) out[i] = full[i];
}
static inline void sha512_final(sha512_ctx_t *ctx, u8 out[64]) { sha512_final_n(ctx, out, 64); }
static inline void sha384_final(sha512_ctx_t *ctx, u8 out[48]) { sha512_final_n(ctx, out, 48); }

static inline void sha512(const u8 *data, u32 len, u8 out[64]) {
    sha512_ctx_t c; sha512_init(&c); sha512_update(&c, data, len); sha512_final(&c, out);
}
static inline void sha384(const u8 *data, u32 len, u8 out[48]) {
    sha512_ctx_t c; sha384_init(&c); sha512_update(&c, data, len); sha384_final(&c, out);
}

#endif
