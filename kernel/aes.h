#ifndef AES_H
#define AES_H
#include "io.h"

/* ============================================================
 * aes.h -- AES-128, per FIPS-197. This kernel implements exactly the
 * 128-bit key variant (10 rounds, not AES-192's 12 or AES-256's 14) --
 * that's the only key size TLS_RSA_WITH_AES_128_CBC_SHA256 (the one
 * cipher suite kernel/tls.h speaks) ever needs, and a from-scratch
 * implementation earns its keep by not building three key schedules
 * when one is all that's used.
 *
 * A straightforward, table-based implementation: the classic S-box and
 * its inverse as literal 256-byte lookup tables (the "byte substitution
 * that took cryptographers a while to justify mathematically" -- see
 * any AES writeup for the actual GF(2^8) derivation; this file just
 * uses the resulting numbers, same as every AES implementation not
 * trying to also be a side-channel-hardened bitsliced masterpiece).
 * No AES-NI, no constant-time guarantees -- this is correctness-first,
 * matching this kernel's general stance that the CPU it runs on
 * (whatever QEMU or real hardware presents) is not assumed to have any
 * particular instruction set extension.
 * ============================================================ */

static const u8 aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static const u8 aes_inv_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d,
};

/* Round constants for key expansion -- x^(round-1) in GF(2^8), per
 * spec. Only need the first 10 for AES-128's 10 rounds. */
static const u8 aes_rcon[11] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

typedef struct {
    u8 round_key[11][16];   /* 11 round keys for AES-128: the original
                             * key plus one derived per round */
} aes128_ctx_t;

/* GF(2^8) multiplication by 2, reduced modulo the AES polynomial
 * (x^8+x^4+x^3+x+1, i.e. 0x11b) -- the one piece of finite-field
 * arithmetic AES's MixColumns step needs. Every other multiplication
 * MixColumns uses (by 1 or by 3) is built from this via xor/shift, so
 * this one function is the entire "Galois field math" this file needs. */
static inline u8 aes_xtime(u8 x) {
    u8 hi = (u8)(x & 0x80);
    u8 shifted = (u8)(x << 1);
    return hi ? (u8)(shifted ^ 0x1b) : shifted;
}
static inline u8 aes_mul(u8 a, u8 b) {
    u8 result = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) result = (u8)(result ^ a);
        a = aes_xtime(a);
        b = (u8)(b >> 1);
    }
    return result;
}

/* Expands a 16-byte key into 11 round keys. Standard Rijndael key
 * schedule: each new 4-byte word is the previous word XORed with the
 * word 4 positions back, with every 4th word additionally going
 * through RotWord+SubWord+Rcon first. */
static inline void aes128_set_key(aes128_ctx_t *ctx, const u8 key[16]) {
    u8 w[44][4]; /* 44 words = 11 round keys x 4 words each */
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) w[i][j] = key[i * 4 + j];

    for (int i = 4; i < 44; i++) {
        u8 temp[4] = { w[i-1][0], w[i-1][1], w[i-1][2], w[i-1][3] };
        if (i % 4 == 0) {
            u8 t0 = temp[0];
            temp[0] = aes_sbox[temp[1]];
            temp[1] = aes_sbox[temp[2]];
            temp[2] = aes_sbox[temp[3]];
            temp[3] = aes_sbox[t0];
            temp[0] = (u8)(temp[0] ^ aes_rcon[i / 4]);
        }
        for (int j = 0; j < 4; j++) w[i][j] = (u8)(w[i-4][j] ^ temp[j]);
    }

    for (int r = 0; r < 11; r++)
        for (int c = 0; c < 4; c++)
            for (int j = 0; j < 4; j++)
                ctx->round_key[r][c * 4 + j] = w[r * 4 + c][j];
}

static inline void aes_add_round_key(u8 state[16], const u8 rk[16]) {
    for (int i = 0; i < 16; i++) state[i] = (u8)(state[i] ^ rk[i]);
}
static inline void aes_sub_bytes(u8 state[16]) {
    for (int i = 0; i < 16; i++) state[i] = aes_sbox[state[i]];
}
static inline void aes_inv_sub_bytes(u8 state[16]) {
    for (int i = 0; i < 16; i++) state[i] = aes_inv_sbox[state[i]];
}

/* State is stored column-major (state[col*4+row]), matching the spec's
 * own 4x4 byte-matrix layout when the 16 input bytes are read in order. */
static inline void aes_shift_rows(u8 s[16]) {
    u8 t;
    /* row 1: shift left by 1 */
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    /* row 2: shift left by 2 */
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    /* row 3: shift left by 3 (== shift right by 1) */
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}
static inline void aes_inv_shift_rows(u8 s[16]) {
    u8 t;
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
}

static inline void aes_mix_columns(u8 s[16]) {
    for (int c = 0; c < 4; c++) {
        u8 *col = s + c * 4;
        u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = (u8)(aes_mul(a0,2) ^ aes_mul(a1,3) ^ a2 ^ a3);
        col[1] = (u8)(a0 ^ aes_mul(a1,2) ^ aes_mul(a2,3) ^ a3);
        col[2] = (u8)(a0 ^ a1 ^ aes_mul(a2,2) ^ aes_mul(a3,3));
        col[3] = (u8)(aes_mul(a0,3) ^ a1 ^ a2 ^ aes_mul(a3,2));
    }
}
static inline void aes_inv_mix_columns(u8 s[16]) {
    for (int c = 0; c < 4; c++) {
        u8 *col = s + c * 4;
        u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = (u8)(aes_mul(a0,14) ^ aes_mul(a1,11) ^ aes_mul(a2,13) ^ aes_mul(a3,9));
        col[1] = (u8)(aes_mul(a0,9) ^ aes_mul(a1,14) ^ aes_mul(a2,11) ^ aes_mul(a3,13));
        col[2] = (u8)(aes_mul(a0,13) ^ aes_mul(a1,9) ^ aes_mul(a2,14) ^ aes_mul(a3,11));
        col[3] = (u8)(aes_mul(a0,11) ^ aes_mul(a1,13) ^ aes_mul(a2,9) ^ aes_mul(a3,14));
    }
}

/* Encrypts exactly one 16-byte block in place. The textbook AES round
 * structure: an initial key whitening, 9 full rounds, then a final
 * round that skips MixColumns (per spec -- the last round is always
 * shaped slightly differently). */
static inline void aes128_encrypt_block(const aes128_ctx_t *ctx, u8 block[16]) {
    aes_add_round_key(block, ctx->round_key[0]);
    for (int round = 1; round <= 9; round++) {
        aes_sub_bytes(block);
        aes_shift_rows(block);
        aes_mix_columns(block);
        aes_add_round_key(block, ctx->round_key[round]);
    }
    aes_sub_bytes(block);
    aes_shift_rows(block);
    aes_add_round_key(block, ctx->round_key[10]);
}

static inline void aes128_decrypt_block(const aes128_ctx_t *ctx, u8 block[16]) {
    aes_add_round_key(block, ctx->round_key[10]);
    aes_inv_shift_rows(block);
    aes_inv_sub_bytes(block);
    for (int round = 9; round >= 1; round--) {
        aes_add_round_key(block, ctx->round_key[round]);
        aes_inv_mix_columns(block);
        aes_inv_shift_rows(block);
        aes_inv_sub_bytes(block);
    }
    aes_add_round_key(block, ctx->round_key[0]);
}

/* CBC mode: each block is XORed with the previous ciphertext block (or
 * the IV, for the first one) before encrypting -- the standard way to
 * turn a block cipher into something that doesn't leak "these two
 * plaintext blocks were identical" the way naive ECB does. `len` must
 * be a multiple of 16; the caller (kernel/tls.h) is responsible for
 * PKCS#7 padding before calling this, same division of labor as any
 * CBC-mode library. Operates in place; `iv` is consumed (read) but not
 * modified -- pass a copy if the caller still needs the original IV
 * afterward. */
static inline void aes128_cbc_encrypt(const aes128_ctx_t *ctx, const u8 iv[16], u8 *data, u32 len) {
    u8 prev[16];
    for (int i = 0; i < 16; i++) prev[i] = iv[i];
    for (u32 off = 0; off < len; off += 16) {
        for (int i = 0; i < 16; i++) data[off + i] = (u8)(data[off + i] ^ prev[i]);
        aes128_encrypt_block(ctx, data + off);
        for (int i = 0; i < 16; i++) prev[i] = data[off + i];
    }
}
static inline void aes128_cbc_decrypt(const aes128_ctx_t *ctx, const u8 iv[16], u8 *data, u32 len) {
    u8 prev[16], cur_ct[16];
    for (int i = 0; i < 16; i++) prev[i] = iv[i];
    for (u32 off = 0; off < len; off += 16) {
        for (int i = 0; i < 16; i++) cur_ct[i] = data[off + i]; /* save before decrypting in place */
        aes128_decrypt_block(ctx, data + off);
        for (int i = 0; i < 16; i++) data[off + i] = (u8)(data[off + i] ^ prev[i]);
        for (int i = 0; i < 16; i++) prev[i] = cur_ct[i];
    }
}

#endif
