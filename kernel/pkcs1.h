#ifndef PKCS1_H
#define PKCS1_H
#include "io.h"
#include "bignum.h"
#include "sha256.h"
#include "sha512.h"

/* ============================================================
 * pkcs1.h -- the OTHER direction of PKCS#1 v1.5: padding a short
 * secret up to the RSA modulus's exact byte width before encrypting it
 * with the server's PUBLIC key, for ClientKeyExchange. (x509.h handles
 * the direction this file doesn't: unpadding a signature after the
 * public-key operation reveals it.)
 *
 * The padding shape for encryption is 0x00 0x02 <nonzero random pad
 * bytes, at least 8> 0x00 <the actual secret> -- superficially similar
 * to the 0x00 0x01 0xFF...0xFF 0x00 shape used for signatures, but
 * deliberately different (the 0x02 block type, and *random* nonzero
 * padding instead of a fixed 0xFF run) since encryption padding and
 * signature padding serve different purposes and RFC 2313 gives them
 * different block types precisely so the two are never confusable.
 * ============================================================ */

/* Pads `secret` (secret_len bytes) into a PKCS#1 v1.5 EME-PKCS1-v1_5
 * encryption block exactly `modulus_len` bytes wide, then RSA-encrypts
 * it (raw public-key operation: block^exponent mod modulus) and writes
 * the result -- also exactly modulus_len bytes -- to `out`. Returns 1
 * on success, 0 if secret_len leaves no room for the mandatory padding
 * (needs at least 11 bytes of overhead: 0x00 0x02, 8+ pad bytes, 0x00).
 *
 * `rand_byte` is a caller-supplied source of non-zero random bytes for
 * the padding -- passed as a function pointer rather than this file
 * reaching for its own randomness source, since "where do random bytes
 * come from" is an environment-specific question (kernel/tls.h answers
 * it using whatever this kernel's entropy sources are) that a generic
 * padding routine has no business deciding on its own. */
typedef u8 (*pkcs1_rand_byte_fn)(void);

/* PKCS#1 v1.5's DigestInfo prefix for SHA-256 -- the DER encoding of
 * SEQUENCE { SEQUENCE { OID sha256, NULL }, OCTET STRING (32 bytes) },
 * everything up through the OCTET STRING's own tag+length. Verified
 * against a real RSA signature produced and "publicly decrypted"
 * during development, not hand-derived from the ASN.1 spec (see
 * kernel/x509.h's OID comment for the same cross-checking philosophy). */
static const u8 PKCS1_SHA256_DIGESTINFO_PREFIX[] = {
    0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20
};

/* Verifies an RSA-PKCS1v1.5-SHA256 signature over an arbitrary blob of
 * already-hashed... no, over the raw data itself (this function hashes
 * it internally) -- the one signature-checking primitive both
 * kernel/x509.h (verifying a certificate against its issuer) and
 * kernel/tls.h (verifying a ServerKeyExchange message against the
 * leaf certificate's key) need, factored out here so the actual
 * "unwrap the RSA public-key operation and check the PKCS#1 padding
 * shape" logic exists exactly once. Returns 1 only if every check
 * passes: correct padding shape, correct DigestInfo prefix, and the
 * recovered hash bytes equal SHA-256(data) exactly. */
/* The other two DigestInfo prefixes real certificate chains need. Same
 * shape as the SHA-256 one above: SEQUENCE { SEQUENCE { OID <hash>, NULL },
 * OCTET STRING (<digest length> bytes) } -- only the OID's last byte
 * (…04 02 02 = sha384, …04 02 03 = sha512) and the lengths change. Checked
 * against real signatures (the host test in tools/test/), not derived by hand. */
static const u8 PKCS1_SHA384_DIGESTINFO_PREFIX[] = {
    0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30
};
static const u8 PKCS1_SHA512_DIGESTINFO_PREFIX[] = {
    0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40
};

/* Which hash a signature was made with. Shared by x509.h (a certificate's
 * signatureAlgorithm) and tls.h (a ServerKeyExchange's SignatureAndHashAlgorithm). */
#define PKCS1_HASH_NONE   0
#define PKCS1_HASH_SHA256 1
#define PKCS1_HASH_SHA384 2
#define PKCS1_HASH_SHA512 3

/* Hashes `data` with the named algorithm into `out` (up to 64 bytes) and returns the digest
 * length, or 0 for an unknown algorithm. */
static inline u32 pkcs1_hash(int hash_id, const u8 *data, u32 data_len, u8 out[64]) {
    if (hash_id == PKCS1_HASH_SHA256) { sha256(data, data_len, out); return 32; }
    if (hash_id == PKCS1_HASH_SHA384) { sha384(data, data_len, out); return 48; }
    if (hash_id == PKCS1_HASH_SHA512) { sha512(data, data_len, out); return 64; }
    return 0;
}

/* Verifies an RSASSA-PKCS1-v1_5 signature over `data`, hashing it with
 * `hash_id` -- the one signature-checking primitive both kernel/x509.h
 * (a certificate against its issuer) and kernel/tls.h (a ServerKeyExchange
 * against the leaf's key) need, so the "undo the RSA public-key operation
 * and check the padding shape" logic exists exactly once. Returns 1 only if
 * every check passes: correct padding shape, the DigestInfo prefix for this
 * hash, and recovered digest bytes exactly equal to hash(data). */
static inline int pkcs1_verify(int hash_id, const u8 *data, u32 data_len,
                               const u8 *sig, u32 sig_len,
                               const bignum_t *modulus, u32 exponent) {
    const u8 *prefix; u32 prefix_len;
    if (hash_id == PKCS1_HASH_SHA256)      { prefix = PKCS1_SHA256_DIGESTINFO_PREFIX; prefix_len = (u32)sizeof(PKCS1_SHA256_DIGESTINFO_PREFIX); }
    else if (hash_id == PKCS1_HASH_SHA384) { prefix = PKCS1_SHA384_DIGESTINFO_PREFIX; prefix_len = (u32)sizeof(PKCS1_SHA384_DIGESTINFO_PREFIX); }
    else if (hash_id == PKCS1_HASH_SHA512) { prefix = PKCS1_SHA512_DIGESTINFO_PREFIX; prefix_len = (u32)sizeof(PKCS1_SHA512_DIGESTINFO_PREFIX); }
    else return 0;

    u32 mod_bytes;
    { int bits = bn_bit_length(modulus); mod_bytes = (u32)((bits + 7) / 8); }
    if (mod_bytes == 0 || mod_bytes > 512) return 0;
    if (sig_len != mod_bytes) return 0;

    bignum_t sig_bn, recovered_bn;
    bn_from_bytes_be(&sig_bn, sig, sig_len);
    bn_modpow_u32exp(&recovered_bn, &sig_bn, exponent, modulus);

    u8 recovered[512];
    bn_to_bytes_be(&recovered_bn, recovered, mod_bytes);

    if (mod_bytes < 11) return 0;
    if (recovered[0] != 0x00 || recovered[1] != 0x01) return 0;
    u32 i = 2;
    while (i < mod_bytes && recovered[i] == 0xFF) i++;
    if (i < 10) return 0;
    if (i >= mod_bytes || recovered[i] != 0x00) return 0;
    i++;

    u8 expected_hash[64];
    u32 hash_len = pkcs1_hash(hash_id, data, data_len, expected_hash);
    u32 digestinfo_len = mod_bytes - i;
    if (digestinfo_len != prefix_len + hash_len) return 0;
    for (u32 j = 0; j < prefix_len; j++) {
        if (recovered[i + j] != prefix[j]) return 0;
    }
    for (u32 j = 0; j < hash_len; j++) {
        if (recovered[i + prefix_len + j] != expected_hash[j]) return 0;
    }
    return 1;
}

/* The original SHA-256-only entry point, kept so existing callers read the same. */
static inline int pkcs1_verify_sha256(const u8 *data, u32 data_len,
                                        const u8 *sig, u32 sig_len,
                                        const bignum_t *modulus, u32 exponent) {
    return pkcs1_verify(PKCS1_HASH_SHA256, data, data_len, sig, sig_len, modulus, exponent);
}

static inline int pkcs1_encrypt(const u8 *secret, u32 secret_len,
                                  u32 modulus_len,
                                  const bignum_t *modulus, u32 exponent,
                                  pkcs1_rand_byte_fn rand_byte,
                                  u8 *out) {
    if (modulus_len < 11 || secret_len > modulus_len - 11) return 0;
    if (modulus_len > 512) return 0; /* matches x509.h's signature buffer
                                      * cap -- nothing bigger than
                                      * RSA-4096 is supported anywhere
                                      * in this client */

    u8 block[512];
    u32 pad_len = modulus_len - secret_len - 3;

    block[0] = 0x00;
    block[1] = 0x02;
    for (u32 i = 0; i < pad_len; i++) {
        u8 b;
        do { b = rand_byte(); } while (b == 0x00); /* padding bytes must be non-zero --
                                                     * a zero byte here would look like
                                                     * the block's own 0x00 separator */
        block[2 + i] = b;
    }
    block[2 + pad_len] = 0x00;
    for (u32 i = 0; i < secret_len; i++) block[3 + pad_len + i] = secret[i];

    bignum_t block_bn, result_bn;
    bn_from_bytes_be(&block_bn, block, modulus_len);
    bn_modpow_u32exp(&result_bn, &block_bn, exponent, modulus);
    bn_to_bytes_be(&result_bn, out, modulus_len);
    return 1;
}

#endif
