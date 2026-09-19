#ifndef BIGNUM_H
#define BIGNUM_H
#include "io.h"

typedef unsigned long long u64;
typedef long long i64;

/* ============================================================
 * bignum.h -- just enough arbitrary-precision unsigned integer math to
 * do one thing: modular exponentiation for RSA's public-key operation
 * (encrypting a premaster secret with a server's public key, and
 * verifying a certificate signature -- both of which are "raise a
 * number to the public exponent, mod n" and nothing fancier). This
 * kernel's TLS client never does an RSA *private*-key operation --
 * that would need the server's or a CA's private key, which obviously
 * nobody outside them has -- so there's no need for the more delicate
 * machinery (CRT, Montgomery ladders defending against timing attacks
 * on a secret exponent) a general RSA library would carry. The public
 * exponent is small (65537 in every certificate this kernel will ever
 * see) and public anyway, so plain schoolbook square-and-multiply,
 * with a plain schoolbook long-division for the modular reduction
 * after every multiply, is both correct and fast enough: modpow with a
 * 17-bit exponent is at most ~17 squarings and one multiply, done once
 * per TLS handshake, not in a hot loop.
 *
 * A bignum here is a fixed-size array of BIGNUM_LIMBS 32-bit "limbs",
 * little-endian (limb[0] is the least significant). BIGNUM_LIMBS is
 * sized for RSA-4096 (4096 bits / 32 = 128 limbs) with headroom, since
 * some certificate chains still use keys that large; RSA-2048 (the
 * common case) uses only the first 64 limbs, and the rest sit at zero.
 * ============================================================ */

#define BIGNUM_LIMBS 130    /* 4160 bits -- covers RSA-4096 with a
                            * couple of limbs of slack for intermediate
                            * values that briefly overflow during
                            * multiplication before reduction */

typedef struct {
    u32 limb[BIGNUM_LIMBS];
} bignum_t;

static inline void bn_zero(bignum_t *a) {
    for (int i = 0; i < BIGNUM_LIMBS; i++) a->limb[i] = 0;
}

/* Loads a big-endian byte buffer (the natural wire/DER format for RSA
 * moduli and signatures) into a bignum. `len` bytes, most significant
 * byte first -- exactly how ASN.1 INTEGER and TLS's opaque<> byte
 * vectors both represent big numbers. */
static inline void bn_from_bytes_be(bignum_t *a, const u8 *data, u32 len) {
    bn_zero(a);
    for (u32 i = 0; i < len; i++) {
        u32 byte_index = len - 1 - i; /* position from the start of data */
        u32 limb_index = i / 4;
        u32 shift = (i % 4) * 8;
        if (limb_index < BIGNUM_LIMBS) {
            a->limb[limb_index] |= ((u32)data[byte_index]) << shift;
        }
    }
}

/* Writes a bignum out as big-endian bytes, exactly `out_len` bytes
 * (zero-padded on the left if the number is smaller -- RSA operations
 * always produce a result the same byte-length as the modulus, and
 * callers rely on that fixed width). */
static inline void bn_to_bytes_be(const bignum_t *a, u8 *out, u32 out_len) {
    for (u32 i = 0; i < out_len; i++) {
        u32 byte_index = out_len - 1 - i;
        u32 limb_index = i / 4;
        u32 shift = (i % 4) * 8;
        out[byte_index] = (limb_index < BIGNUM_LIMBS) ? (u8)(a->limb[limb_index] >> shift) : 0;
    }
}

/* Returns the index one past the most significant non-zero limb (i.e.
 * the "used length" in limbs), or 0 if the number is zero. Every
 * multi-precision routine below uses this to avoid wasting time
 * iterating over leading zero limbs of whichever operand is smaller
 * than BIGNUM_LIMBS's full width (which, for a 2048-bit modulus in a
 * 4160-bit-capacity bignum, is most of it). */
static inline int bn_used_limbs(const bignum_t *a) {
    for (int i = BIGNUM_LIMBS - 1; i >= 0; i--) {
        if (a->limb[i] != 0) return i + 1;
    }
    return 0;
}

/* Three-way compare: -1 if a<b, 0 if equal, 1 if a>b. */
static inline int bn_cmp(const bignum_t *a, const bignum_t *b) {
    for (int i = BIGNUM_LIMBS - 1; i >= 0; i--) {
        if (a->limb[i] != b->limb[i]) return (a->limb[i] < b->limb[i]) ? -1 : 1;
    }
    return 0;
}

/* result = a - b, assuming a >= b (callers are responsible for checking
 * that -- this function has no way to signal "went negative" other
 * than wrapping, which would silently corrupt every caller's math, so
 * it simply isn't allowed to happen). Standard borrow-propagating
 * subtraction. */
static inline void bn_sub(bignum_t *result, const bignum_t *a, const bignum_t *b) {
    i32 borrow = 0;
    for (int i = 0; i < BIGNUM_LIMBS; i++) {
        i64 diff = (i64)a->limb[i] - (i64)b->limb[i] - borrow;
        if (diff < 0) { diff += ((i64)1 << 32); borrow = 1; } else { borrow = 0; }
        result->limb[i] = (u32)diff;
    }
}

/* result (2*BIGNUM_LIMBS limbs, so it can hold the full-width product
 * of two BIGNUM_LIMBS-limb numbers without overflowing) = a * b.
 * Schoolbook long multiplication -- O(n^2) in the number of limbs, but
 * n here is at most ~128, and this runs a handful of times per
 * handshake, not per packet. */
static inline void bn_mul_wide(u32 result[2 * BIGNUM_LIMBS], const bignum_t *a, const bignum_t *b) {
    for (int i = 0; i < 2 * BIGNUM_LIMBS; i++) result[i] = 0;
    int a_len = bn_used_limbs(a);
    int b_len = bn_used_limbs(b);
    for (int i = 0; i < a_len; i++) {
        if (a->limb[i] == 0) continue;
        u64 carry = 0;
        for (int j = 0; j < b_len; j++) {
            u64 prod = (u64)a->limb[i] * (u64)b->limb[j] + (u64)result[i + j] + carry;
            result[i + j] = (u32)prod;
            carry = prod >> 32;
        }
        int k = i + b_len;
        while (carry) {
            u64 sum = (u64)result[k] + carry;
            result[k] = (u32)sum;
            carry = sum >> 32;
            k++;
        }
    }
}

/* Reduces a 2*BIGNUM_LIMBS-wide value modulo `mod`, leaving the
 * (BIGNUM_LIMBS-wide) remainder in `result`. Plain schoolbook binary
 * long division: repeatedly find the largest shift of `mod` that still
 * fits under the remaining dividend, subtract it, record a 1 bit,
 * shift down, repeat -- the exact same algorithm taught for long
 * division in decimal, just base 2 and applied to a value that can be
 * thousands of bits wide. Correct and simple; not fast (Barrett or
 * Montgomery reduction would be, at the cost of a much subtler
 * implementation this project has no need to risk getting wrong for a
 * once-per-handshake operation). */
/* Highest set-bit index across a wide (2*BIGNUM_LIMBS-limb) buffer, or
 * -1 if it's all zero. Used by bn_mod_wide() to skip however many
 * leading zero bits separate the buffer's fixed maximum capacity from
 * whatever the actual product's real bit-length is -- for a 2048-bit
 * RSA modulus (the common case), the product of two <2048-bit numbers
 * is at most 4096 bits, well under this buffer's 8320-bit capacity, and
 * skipping straight to real data instead of counting down from the
 * theoretical maximum is the difference between a modpow that finishes
 * in a reasonable fraction of a second and one that doesn't. */
static inline int bn_wide_bit_length(const u32 wide[2 * BIGNUM_LIMBS]) {
    for (int limb = 2 * BIGNUM_LIMBS - 1; limb >= 0; limb--) {
        if (wide[limb] != 0) {
            for (int bit = 31; bit >= 0; bit--) {
                if (wide[limb] & (1u << bit)) return limb * 32 + bit;
            }
        }
    }
    return -1;
}

/* Same idea for a plain bignum_t -- used to size the working limb count
 * for the divisor (mod) side of the reduction, so the inner "shift the
 * remainder left by one" step only touches the limbs that can possibly
 * be non-zero instead of always all BIGNUM_LIMBS of them. */
static inline int bn_bit_length(const bignum_t *a) {
    int used = bn_used_limbs(a);
    if (used == 0) return 0;
    u32 top = a->limb[used - 1];
    int bit = 31;
    while (bit >= 0 && !(top & (1u << bit))) bit--;
    return (used - 1) * 32 + bit + 1;
}

static inline void bn_mod_wide(bignum_t *result, const u32 wide[2 * BIGNUM_LIMBS], const bignum_t *mod) {
    bignum_t rem;
    bn_zero(&rem);

    int top_bit = bn_wide_bit_length(wide);
    /* Work limb count: enough to hold both the remainder (which never
     * exceeds mod, by construction below) and the shifted-in bits from
     * wide -- mod's own limb count plus one guard limb for the
     * shift-then-compare step is always sufficient, and staying within
     * that range (instead of BIGNUM_LIMBS) is what actually makes this
     * fast for moduli much smaller than this type's 4096-bit capacity. */
    int work_limbs = bn_used_limbs(mod) + 1;
    if (work_limbs > BIGNUM_LIMBS) work_limbs = BIGNUM_LIMBS;

    for (int bit = top_bit; bit >= 0; bit--) {
        u32 carry = 0;
        for (int i = 0; i < work_limbs; i++) {
            u32 new_carry = rem.limb[i] >> 31;
            rem.limb[i] = (rem.limb[i] << 1) | carry;
            carry = new_carry;
        }
        u32 word = wide[bit / 32];
        u32 in_bit = (word >> (bit % 32)) & 1;
        rem.limb[0] |= in_bit;

        if (bn_cmp(&rem, mod) >= 0) {
            bignum_t tmp;
            bn_sub(&tmp, &rem, mod);
            rem = tmp;
        }
    }
    *result = rem;
}

/* result = (a * b) mod m -- the one composite operation modpow needs,
 * built from the three primitives above. */
static inline void bn_mulmod(bignum_t *result, const bignum_t *a, const bignum_t *b, const bignum_t *m) {
    u32 wide[2 * BIGNUM_LIMBS];
    bn_mul_wide(wide, a, b);
    bn_mod_wide(result, wide, m);
}

/* result = base^exp mod m, via left-to-right square-and-multiply.
 * `exp` is given as a plain u32 rather than a bignum -- every exponent
 * this kernel's TLS client ever raises anything to is a small public
 * RSA exponent (65537, universally, in every certificate that will
 * ever cross this code path), never a multi-limb secret exponent, so
 * there's no reason for this function's signature to pretend
 * otherwise. */
static inline void bn_modpow_u32exp(bignum_t *result, const bignum_t *base, u32 exp, const bignum_t *m) {
    bignum_t acc;
    bn_zero(&acc);
    acc.limb[0] = 1; /* acc = 1 */

    bignum_t b = *base;
    /* Reduce base mod m first -- callers may pass a base that's
     * already < m (the normal case for RSA, since the message is
     * always smaller than the modulus), but this keeps the function
     * correct even if that invariant is ever violated. */
    if (bn_cmp(&b, m) >= 0) {
        u32 wide[2 * BIGNUM_LIMBS];
        for (int i = 0; i < BIGNUM_LIMBS; i++) wide[i] = b.limb[i];
        for (int i = BIGNUM_LIMBS; i < 2 * BIGNUM_LIMBS; i++) wide[i] = 0;
        bn_mod_wide(&b, wide, m);
    }

    /* Find the highest set bit of exp so we start the square-and-
     * multiply loop at the right place instead of wasting squarings on
     * leading zero bits. */
    int top_bit = -1;
    for (int i = 31; i >= 0; i--) {
        if (exp & (1u << i)) { top_bit = i; break; }
    }

    for (int i = top_bit; i >= 0; i--) {
        bignum_t sq;
        bn_mulmod(&sq, &acc, &acc, m);
        acc = sq;
        if (exp & (1u << i)) {
            bignum_t mul;
            bn_mulmod(&mul, &acc, &b, m);
            acc = mul;
        }
    }
    *result = acc;
}

/* result = a + b, full multi-precision add with carry propagation.
 * Unlike bn_sub(), this never needs an ordering precondition -- addition
 * of two non-negative numbers always has a well-defined non-negative
 * result (mod overflowing the type's own capacity, which none of this
 * client's actual values ever do: RSA operands are already reduced mod
 * a modulus well within BIGNUM_LIMBS, and X25519 field elements are
 * smaller still). */
static inline void bn_add(bignum_t *result, const bignum_t *a, const bignum_t *b) {
    u32 carry = 0;
    for (int i = 0; i < BIGNUM_LIMBS; i++) {
        u64 sum = (u64)a->limb[i] + (u64)b->limb[i] + carry;
        result->limb[i] = (u32)sum;
        carry = (u32)(sum >> 32);
    }
}

/* result = (a + b) mod m. Valid whenever a<m and b<m (true for every
 * caller in this client): then a+b<2m, so at most one conditional
 * subtraction of m is ever needed to bring it back into range --
 * cheaper than a full division-based reduction, and correct precisely
 * because both inputs are already-reduced field elements, not
 * arbitrary sums. */
static inline void bn_addmod(bignum_t *result, const bignum_t *a, const bignum_t *b, const bignum_t *m) {
    bignum_t sum;
    bn_add(&sum, a, b);
    if (bn_cmp(&sum, m) >= 0) {
        bignum_t reduced;
        bn_sub(&reduced, &sum, m);
        *result = reduced;
    } else {
        *result = sum;
    }
}

/* result = (a - b) mod m, for a,b both already < m. When a>=b this is
 * just bn_sub(); when a<b, the mathematically correct answer is
 * m-(b-a), computed the same way (as a subtraction that itself never
 * goes negative, since b-a<m by the same already-reduced-inputs
 * argument as bn_addmod above). Field subtraction shows up constantly
 * in X25519's Montgomery ladder (B=x2-z2, D=x3-z3, E=AA-BB, ...), so
 * this -- not bn_sub()'s "caller guarantees a>=b" contract -- is the
 * function every one of those call sites actually wants. */
static inline void bn_submod(bignum_t *result, const bignum_t *a, const bignum_t *b, const bignum_t *m) {
    if (bn_cmp(a, b) >= 0) {
        bn_sub(result, a, b);
    } else {
        bignum_t diff;
        bn_sub(&diff, b, a);       /* b-a, positive since b>a here */
        bn_sub(result, m, &diff);  /* m-(b-a) */
    }
}

/* Same square-and-multiply as bn_modpow_u32exp, generalized to a
 * full-width bignum exponent instead of a plain u32 -- needed for
 * X25519's modular inverse step (raising to p-2, a 255-bit exponent,
 * via Fermat's little theorem), which is the one place in this client
 * an exponent doesn't fit comfortably in 32 bits. bn_modpow_u32exp()
 * itself stays as its own function rather than becoming a thin wrapper
 * around this one, since RSA's small public exponent is the overwhelmingly
 * common case and deserves not to pay for iterating a multi-hundred-bit
 * exponent representation just to process 17 significant bits of it. */
static inline void bn_modpow(bignum_t *result, const bignum_t *base, const bignum_t *exp, const bignum_t *m) {
    bignum_t acc;
    bn_zero(&acc);
    acc.limb[0] = 1;

    bignum_t b = *base;
    if (bn_cmp(&b, m) >= 0) {
        u32 wide[2 * BIGNUM_LIMBS];
        for (int i = 0; i < BIGNUM_LIMBS; i++) wide[i] = b.limb[i];
        for (int i = BIGNUM_LIMBS; i < 2 * BIGNUM_LIMBS; i++) wide[i] = 0;
        bn_mod_wide(&b, wide, m);
    }

    int top_bit = bn_bit_length(exp) - 1;
    for (int i = top_bit; i >= 0; i--) {
        bignum_t sq;
        bn_mulmod(&sq, &acc, &acc, m);
        acc = sq;
        int limb_idx = i / 32, bit_idx = i % 32;
        if (exp->limb[limb_idx] & (1u << bit_idx)) {
            bignum_t mul;
            bn_mulmod(&mul, &acc, &b, m);
            acc = mul;
        }
    }
    *result = acc;
}

#endif
