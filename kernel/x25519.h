#ifndef X25519_H
#define X25519_H
#include "io.h"
#include "bignum.h"

/* ============================================================
 * x25519.h -- Curve25519 Diffie-Hellman, per RFC 7748. This is the key
 * exchange every modern TLS server actually wants: ECDHE_RSA cipher
 * suites use this curve (or occasionally NIST P-256, which this file
 * does not implement -- see kernel/tls.h's cipher suite negotiation
 * comment for why sticking to X25519 alone is a deliberate, not lazy,
 * choice) to derive a fresh shared secret every connection, giving
 * forward secrecy that plain RSA key transport never could. Discovered
 * the hard way during this client's development: a modern
 * security-conscious server flatly refused a TLS_RSA_* handshake with
 * "handshake failure" -- non-forward-secret key exchange has been
 * falling out of favor industry-wide for years, and X25519 support is
 * what actually lets this client talk to the real web.
 *
 * Implemented as the textbook Montgomery ladder over GF(2^255-19),
 * built on the same bignum_t/bn_mulmod machinery kernel/bignum.h
 * already uses for RSA -- reusing already-tested modular arithmetic
 * instead of writing Curve25519-specific fast-reduction tricks (the
 * usual reason real implementations avoid generic bignum code here is
 * speed; this client's "once per handshake, not in a hot loop" stance
 * on RSA applies equally well to a single scalar multiplication).
 * ============================================================ */

#define X25519_A24_CONSTANT 121665u   /* (486662-2)/4, Curve25519's own
                                       * curve-equation constant -- this
                                       * exact value is baked into
                                       * RFC 7748's ladder step formula,
                                       * not something a caller ever
                                       * needs to know or change */

/* p = 2^255 - 19, the field Curve25519 lives over. */
static inline void x25519_prime(bignum_t *p) {
    u8 p_bytes[32] = {
        0x7f,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xed,
    };
    bn_from_bytes_be(p, p_bytes, 32);
}

/* Applies RFC 7748's mandatory "clamping" to a raw 32-byte scalar
 * before using it as a Curve25519 private key: clear the low 3 bits
 * (forces the scalar to a multiple of the curve's cofactor, 8 --
 * defends against small-subgroup attacks), clear the top bit (keeps
 * the scalar under 2^255), and set the second-highest bit (a
 * side-channel-timing consideration from the original design that
 * also conveniently pins the ladder's iteration count). Every X25519
 * implementation does exactly this, unconditionally -- it isn't
 * optional, and skipping it isn't "simpler," it's a different and
 * weaker function that shouldn't be called X25519. */
static inline void x25519_clamp(u8 k[32]) {
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;
}

/* One Curve25519 scalar multiplication: out = scalar * u_point, all as
 * 32-byte little-endian values (the wire format RFC 7748 and TLS's key
 * share extensions both use -- note this is the OPPOSITE byte order
 * from the big-endian convention every RSA-related function in this
 * client's other headers uses; Curve25519 inherited little-endian from
 * its original reference implementation, and fighting that convention
 * would only make this file harder to cross-check against RFC 7748's
 * own worked examples). `scalar` is clamped internally -- callers pass
 * their raw private key bytes, not a pre-clamped value. */
static inline void x25519_scalarmult(u8 out[32], const u8 scalar[32], const u8 u_point[32]) {
    u8 k[32];
    for (int i = 0; i < 32; i++) k[i] = scalar[i];
    x25519_clamp(k);

    bignum_t p;
    x25519_prime(&p);

    /* Field elements loaded from little-endian wire bytes -- reverse
     * into the big-endian byte order bn_from_bytes_be() expects, since
     * this client's bignum type is byte-order-agnostic in its limb
     * storage but its *loading functions* commit to big-endian input,
     * matching every other caller (RSA moduli, DER INTEGERs) that isn't
     * this one curve. */
    u8 u_be[32];
    for (int i = 0; i < 32; i++) u_be[i] = u_point[31 - i];
    bignum_t x1;
    bn_from_bytes_be(&x1, u_be, 32);

    bignum_t x2, z2, x3, z3;
    bn_zero(&x2); x2.limb[0] = 1;   /* x2 = 1 */
    bn_zero(&z2);                   /* z2 = 0 */
    x3 = x1;                        /* x3 = u */
    bn_zero(&z3); z3.limb[0] = 1;   /* z3 = 1 */

    int swap = 0;
    for (int t = 254; t >= 0; t--) {
        int kt = (k[t / 8] >> (t % 8)) & 1;
        swap ^= kt;
        if (swap) {
            bignum_t tmp;
            tmp = x2; x2 = x3; x3 = tmp;
            tmp = z2; z2 = z3; z3 = tmp;
        }
        swap = kt;

        bignum_t A, AA, B, BB, E, C, D, DA, CB;
        bn_addmod(&A, &x2, &z2, &p);
        bn_mulmod(&AA, &A, &A, &p);
        bn_submod(&B, &x2, &z2, &p);
        bn_mulmod(&BB, &B, &B, &p);
        bn_submod(&E, &AA, &BB, &p);
        bn_addmod(&C, &x3, &z3, &p);
        bn_submod(&D, &x3, &z3, &p);
        bn_mulmod(&DA, &D, &A, &p);
        bn_mulmod(&CB, &C, &B, &p);

        bignum_t sum_da_cb, diff_da_cb;
        bn_addmod(&sum_da_cb, &DA, &CB, &p);
        bn_mulmod(&x3, &sum_da_cb, &sum_da_cb, &p);            /* x3 = (DA+CB)^2 */

        bn_submod(&diff_da_cb, &DA, &CB, &p);
        bignum_t diff_sq;
        bn_mulmod(&diff_sq, &diff_da_cb, &diff_da_cb, &p);
        bn_mulmod(&z3, &x1, &diff_sq, &p);                      /* z3 = x1*(DA-CB)^2 */

        bn_mulmod(&x2, &AA, &BB, &p);                           /* x2 = AA*BB */

        bignum_t a24_e, a24_bn;
        bn_zero(&a24_bn); a24_bn.limb[0] = X25519_A24_CONSTANT;
        bn_mulmod(&a24_e, &a24_bn, &E, &p);
        bignum_t aa_plus_a24e;
        bn_addmod(&aa_plus_a24e, &AA, &a24_e, &p);
        bn_mulmod(&z2, &E, &aa_plus_a24e, &p);                  /* z2 = E*(AA+a24*E) */
    }
    if (swap) {
        bignum_t tmp;
        tmp = x2; x2 = x3; x3 = tmp;
        tmp = z2; z2 = z3; z3 = tmp;
    }

    /* result = x2 * z2^(p-2) mod p -- the modular inverse of z2, via
     * Fermat's little theorem (valid since p is prime: z2^(p-1)==1, so
     * z2^(p-2) is z2's own inverse). This is the one place this
     * function needs a full-width-exponent modpow instead of RSA's
     * small public exponent. */
    bignum_t p_minus_2, two;
    bn_zero(&two); two.limb[0] = 2;
    bn_sub(&p_minus_2, &p, &two);
    bignum_t z2_inv, result;
    bn_modpow(&z2_inv, &z2, &p_minus_2, &p);
    bn_mulmod(&result, &x2, &z2_inv, &p);

    u8 result_be[32];
    bn_to_bytes_be(&result, result_be, 32);
    for (int i = 0; i < 32; i++) out[i] = result_be[31 - i]; /* back to little-endian for the wire */
}

/* Computes our own public key from a raw private key: scalarmult
 * against the curve's standard base point, u=9 (a single non-zero byte
 * followed by 31 zero bytes, little-endian -- RFC 7748's defined
 * starting point for every Curve25519 key generation). */
static inline void x25519_derive_public(u8 out_public[32], const u8 private_key[32]) {
    u8 base[32];
    base[0] = 9;
    for (int i = 1; i < 32; i++) base[i] = 0;
    x25519_scalarmult(out_public, private_key, base);
}

#endif
