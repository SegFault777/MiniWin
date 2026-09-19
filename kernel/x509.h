#ifndef X509_H
#define X509_H
#include "io.h"
#include "asn1.h"
#include "bignum.h"
#include "sha256.h"
#include "pkcs1.h"

/* ============================================================
 * x509.h -- turns a DER-encoded X.509 certificate into the handful of
 * fields kernel/tls.h actually needs to establish trust: who signed
 * it, what its public key is, whether it's still within its validity
 * window, whether it claims to be a CA, and whether it covers the
 * hostname we're trying to reach. Built entirely on asn1.h's generic
 * TLV walker -- this file just knows the specific *shape* of a
 * Certificate (RFC 5280's ASN.1 module), field by field, in the exact
 * order the spec lays them out.
 *
 * Only RSA + SHA-256 signatures are understood (matching the rest of
 * this TLS client's single-cipher-suite scope) -- a certificate signed
 * with anything else (ECDSA, SHA-1, SHA-384, ...) is parsed far enough
 * to be recognized and rejected with a clear reason, not silently
 * mishandled.
 * ============================================================ */

/* DER-encoded OID byte sequences this parser needs to recognize.
 * Extracted with a real ASN.1 library and cross-checked against an
 * actual certificate's bytes during development (see the OID constants
 * comment in kernel/tls.h for where these came from) -- not
 * hand-derived from the dotted-decimal arithmetic, which is exactly
 * the kind of off-by-one-byte mistake that fails silently (a
 * non-matching OID just looks like "unsupported algorithm," not a
 * crash) and is easy to miss without cross-checking against a real
 * encoder. */
static const u8 OID_RSA_ENCRYPTION[]      = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
static const u8 OID_SHA256_WITH_RSA[]     = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b};
static const u8 OID_SHA256[]              = {0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01};
static const u8 OID_SUBJECT_ALT_NAME[]    = {0x55,0x1d,0x11};
static const u8 OID_BASIC_CONSTRAINTS[]   = {0x55,0x1d,0x13};
static const u8 OID_COMMON_NAME[]         = {0x55,0x04,0x03};

#define X509_MAX_NAME_LEN 512   /* raw DER bytes of issuer/subject Name --
                                 * kept as opaque bytes for equality
                                 * comparison between a cert's issuer and
                                 * its signer's subject, never decoded
                                 * into a human string except where we
                                 * specifically pull out a CN for
                                 * hostname fallback matching */

typedef struct {
    const u8 *tbs_data;   /* the exact bytes that were signed -- what
                           * gets hashed and checked against the
                           * signature */
    u32 tbs_len;

    u8  issuer[X509_MAX_NAME_LEN];
    u32 issuer_len;
    u8  subject[X509_MAX_NAME_LEN];
    u32 subject_len;

    /* Validity window, packed as YYYYMMDDHHMMSS into a single u64 so
     * "is now between not_before and not_after" is a plain integer
     * comparison -- no calendar math needed anywhere else in this
     * file. */
    u64 not_before;
    u64 not_after;

    bignum_t pubkey_modulus;
    u32 pubkey_exponent;
    int pubkey_valid;   /* 0 if the SPKI algorithm wasn't rsaEncryption --
                         * an ECDSA certificate, say -- in which case
                         * this cert can still be inspected but can
                         * never be used as an issuer (we have no ECDSA
                         * verification) or, if it's the leaf, means
                         * this connection can't proceed */

    u8  signature[512];   /* raw RSA signature bytes -- big enough for
                           * RSA-4096 (512 bytes); anything larger is
                           * outside what this client's bignum_t
                           * (BIGNUM_LIMBS) can represent anyway */
    u32 signature_len;
    int sig_alg_supported;   /* sha256WithRSAEncryption specifically --
                              * anything else and this cert can't be
                              * cryptographically verified by this
                              * client, full stop */

    int is_ca;   /* basicConstraints CA:TRUE -- required for any cert
                 * used as an issuer partway up a chain; irrelevant for
                 * the leaf */

    /* subjectAltName's DNS entries, kept as a pointer into the original
     * cert buffer + length rather than copied out -- x509_matches_hostname()
     * walks this directly. NULL/0 if the extension wasn't present. */
    const u8 *san_ext_value;
    u32 san_ext_len;

    /* Raw CN, for the RFC-6125-sanctioned fallback of matching against
     * the Subject's commonName when there's no SAN extension at all
     * (rare among real CA-issued certs today, but not unheard of, and
     * cheap to support correctly rather than special-case away). */
    u8  cn[256];
    u32 cn_len;
} x509_cert_t;

/* Parses a two-digit-year UTCTime ("YYMMDDHHMMSSZ") or four-digit-year
 * GeneralizedTime ("YYYYMMDDHHMMSSZ") into the packed
 * YYYYMMDDHHMMSS u64 format used throughout this file. UTCTime's
 * 2-digit year follows RFC 5280's pivot rule: 00-49 means 20xx, 50-99
 * means 19xx (a certificate authority backdating a cert to the 1950s
 * is not a case this client needs to get right). */
static inline u64 x509_parse_time(const asn1_tlv_t *t) {
    const u8 *s = t->value;
    u32 len = t->len;
    u32 pos = 0;
    u32 year;

    if (t->tag == ASN1_TAG_UTC_TIME) {
        if (len < 13) return 0;
        u32 yy = (u32)(s[0]-'0')*10 + (u32)(s[1]-'0');
        year = (yy < 50) ? 2000 + yy : 1900 + yy;
        pos = 2;
    } else if (t->tag == ASN1_TAG_GENERALIZED_TIME) {
        if (len < 15) return 0;
        year = (u32)(s[0]-'0')*1000 + (u32)(s[1]-'0')*100 + (u32)(s[2]-'0')*10 + (u32)(s[3]-'0');
        pos = 4;
    } else {
        return 0;
    }

    u32 mon = (u32)(s[pos]-'0')*10 + (u32)(s[pos+1]-'0'); pos += 2;
    u32 day = (u32)(s[pos]-'0')*10 + (u32)(s[pos+1]-'0'); pos += 2;
    u32 hh  = (u32)(s[pos]-'0')*10 + (u32)(s[pos+1]-'0'); pos += 2;
    u32 mi  = (u32)(s[pos]-'0')*10 + (u32)(s[pos+1]-'0'); pos += 2;
    u32 ss  = (u32)(s[pos]-'0')*10 + (u32)(s[pos+1]-'0');

    return (u64)year * 10000000000ull + (u64)mon * 100000000ull + (u64)day * 1000000ull
         + (u64)hh * 10000ull + (u64)mi * 100ull + (u64)ss;
}

/* Walks a Name SEQUENCE (a SET OF SEQUENCE OF AttributeTypeAndValue,
 * per RFC 5280 -- X.509's famously over-general way of saying "a list
 * of RDNs, each a list of type=value pairs") looking specifically for
 * a commonName attribute, and copies its string value into cert->cn.
 * Only used for the SAN-absent fallback case; every other use of
 * issuer/subject in this file treats the Name as opaque bytes. */
static inline void x509_extract_cn(x509_cert_t *cert, const u8 *name_data, u32 name_len) {
    cert->cn_len = 0;
    const u8 *p = name_data;
    u32 remaining = name_len;
    while (remaining > 0) {
        asn1_tlv_t rdn_set;
        if (!asn1_expect(p, remaining, ASN1_TAG_SET, &rdn_set)) return;
        const u8 *rp = rdn_set.value;
        u32 r_remaining = rdn_set.len;
        while (r_remaining > 0) {
            asn1_tlv_t atv_seq;
            if (!asn1_expect(rp, r_remaining, ASN1_TAG_SEQUENCE, &atv_seq)) break;
            asn1_tlv_t oid_t;
            if (asn1_parse_tlv(atv_seq.value, atv_seq.len, &oid_t) &&
                asn1_oid_equals(&oid_t, OID_COMMON_NAME, sizeof(OID_COMMON_NAME))) {
                asn1_tlv_t val_t;
                u32 val_remaining = atv_seq.len - (u32)(oid_t.next - atv_seq.value);
                if (asn1_parse_tlv(oid_t.next, val_remaining, &val_t)) {
                    u32 n = val_t.len < sizeof(cert->cn) - 1 ? val_t.len : sizeof(cert->cn) - 1;
                    for (u32 i = 0; i < n; i++) cert->cn[i] = val_t.value[i];
                    cert->cn_len = n;
                }
            }
            r_remaining -= (u32)(atv_seq.next - rp);
            rp = atv_seq.next;
        }
        remaining -= (u32)(rdn_set.next - p);
        p = rdn_set.next;
    }
}

/* Walks a certificate's Extensions [3] block looking for the ones this
 * client cares about (basicConstraints, subjectAltName); anything else
 * is skipped by its own length, same as everywhere else in this
 * parser. Extensions are OPTIONAL and only present in v3 certificates
 * -- effectively every certificate on the modern web, but this function
 * is simply never called for a cert that omits the block. */
static inline void x509_parse_extensions(x509_cert_t *cert, const u8 *ext_data, u32 ext_len) {
    const u8 *p = ext_data;
    u32 remaining = ext_len;
    while (remaining > 0) {
        asn1_tlv_t ext_seq;
        if (!asn1_expect(p, remaining, ASN1_TAG_SEQUENCE, &ext_seq)) return;

        asn1_tlv_t oid_t;
        if (!asn1_parse_tlv(ext_seq.value, ext_seq.len, &oid_t)) return;
        const u8 *after_oid = oid_t.next;
        u32 after_oid_remaining = ext_seq.len - (u32)(oid_t.next - ext_seq.value);

        /* OPTIONAL critical BOOLEAN may come next -- skip it if present */
        asn1_tlv_t maybe_bool;
        if (asn1_parse_tlv(after_oid, after_oid_remaining, &maybe_bool) && maybe_bool.tag == 0x01) {
            after_oid_remaining -= (u32)(maybe_bool.next - after_oid);
            after_oid = maybe_bool.next;
        }

        /* extnValue OCTET STRING -- its *content* is itself another
         * DER-encoded value specific to the extension type */
        asn1_tlv_t octet;
        if (asn1_expect(after_oid, after_oid_remaining, ASN1_TAG_OCTET_STRING, &octet)) {
            if (asn1_oid_equals(&oid_t, OID_SUBJECT_ALT_NAME, sizeof(OID_SUBJECT_ALT_NAME))) {
                cert->san_ext_value = octet.value;
                cert->san_ext_len = octet.len;
            } else if (asn1_oid_equals(&oid_t, OID_BASIC_CONSTRAINTS, sizeof(OID_BASIC_CONSTRAINTS))) {
                /* BasicConstraints ::= SEQUENCE { cA BOOLEAN DEFAULT FALSE, ... } */
                asn1_tlv_t bc_seq;
                if (asn1_expect(octet.value, octet.len, ASN1_TAG_SEQUENCE, &bc_seq) && bc_seq.len > 0) {
                    asn1_tlv_t ca_bool;
                    if (asn1_parse_tlv(bc_seq.value, bc_seq.len, &ca_bool) && ca_bool.tag == 0x01) {
                        cert->is_ca = (ca_bool.len > 0 && ca_bool.value[0] != 0x00);
                    }
                }
            }
        }

        remaining -= (u32)(ext_seq.next - p);
        p = ext_seq.next;
    }
}

/* Parses one DER-encoded Certificate into `cert`. Returns 1 on success
 * -- meaning "this is well-formed enough to inspect," NOT "this is
 * trustworthy": callers still need x509_verify_signature() and
 * x509_is_valid_now() and x509_matches_hostname() before trusting
 * anything this extracted. Returns 0 for a malformed encoding, or a
 * well-formed one using a field type this parser doesn't handle (an
 * INTEGER where a SEQUENCE was expected, say) -- either way, "cannot
 * safely proceed" rather than guessing. */
static inline int x509_parse(const u8 *der, u32 der_len, x509_cert_t *cert) {
    cert->san_ext_value = 0; cert->san_ext_len = 0;
    cert->is_ca = 0;
    cert->cn_len = 0;
    cert->pubkey_valid = 0;
    cert->sig_alg_supported = 0;

    asn1_tlv_t outer, tbs;
    if (!asn1_expect(der, der_len, ASN1_TAG_SEQUENCE, &outer)) return 0;
    if (!asn1_expect(outer.value, outer.len, ASN1_TAG_SEQUENCE, &tbs)) return 0;

    /* The signed bytes are the tbsCertificate's *entire* TLV encoding
     * (tag + length + value), not just its value -- DER signatures
     * always cover the full re-encodable structure, header included. */
    {
        u32 header_len = (u32)(tbs.value - outer.value);
        cert->tbs_data = outer.value;
        cert->tbs_len = header_len + tbs.len;
    }

    const u8 *p = tbs.value;
    u32 remaining = tbs.len;
    asn1_tlv_t t;

    /* version: OPTIONAL [0] EXPLICIT INTEGER DEFAULT v1. If the next
     * TLV is the context-specific [0] wrapper, skip past it (we don't
     * branch on the version number itself -- every field we care about
     * exists in both v1 and v3, we just also read v3's extensions block
     * later if it's there). */
    if (!asn1_parse_tlv(p, remaining, &t)) return 0;
    if (t.tag == ASN1_TAG_CONTEXT(0)) {
        remaining -= (u32)(t.next - p); p = t.next;
        if (!asn1_parse_tlv(p, remaining, &t)) return 0;
    }
    /* t is serialNumber INTEGER -- skip, we don't need its value */
    remaining -= (u32)(t.next - p); p = t.next;

    /* signature AlgorithmIdentifier (restated at the end of the outer
     * Certificate too, per RFC 5280 4.1.1.2 -- that second copy is the
     * one this parser actually reads, further down) */
    if (!asn1_expect(p, remaining, ASN1_TAG_SEQUENCE, &t)) return 0;
    remaining -= (u32)(t.next - p); p = t.next;

    /* issuer Name -- kept as raw bytes for later issuer/subject byte
     * comparison against the next cert up the chain */
    if (!asn1_expect(p, remaining, ASN1_TAG_SEQUENCE, &t)) return 0;
    {
        u32 full_len = (u32)(t.next - p);
        u32 n = full_len < X509_MAX_NAME_LEN ? full_len : X509_MAX_NAME_LEN;
        for (u32 i = 0; i < n; i++) cert->issuer[i] = p[i];
        cert->issuer_len = n;
    }
    remaining -= (u32)(t.next - p); p = t.next;

    /* validity SEQUENCE { notBefore Time, notAfter Time } */
    if (!asn1_expect(p, remaining, ASN1_TAG_SEQUENCE, &t)) return 0;
    {
        asn1_tlv_t nb, na;
        if (!asn1_parse_tlv(t.value, t.len, &nb)) return 0;
        u32 v_remaining = t.len - (u32)(nb.next - t.value);
        if (!asn1_parse_tlv(nb.next, v_remaining, &na)) return 0;
        cert->not_before = x509_parse_time(&nb);
        cert->not_after = x509_parse_time(&na);
    }
    remaining -= (u32)(t.next - p); p = t.next;

    /* subject Name */
    if (!asn1_expect(p, remaining, ASN1_TAG_SEQUENCE, &t)) return 0;
    {
        u32 full_len = (u32)(t.next - p);
        u32 n = full_len < X509_MAX_NAME_LEN ? full_len : X509_MAX_NAME_LEN;
        for (u32 i = 0; i < n; i++) cert->subject[i] = p[i];
        cert->subject_len = n;
    }
    x509_extract_cn(cert, t.value, t.len);
    remaining -= (u32)(t.next - p); p = t.next;

    /* subjectPublicKeyInfo SEQUENCE { algorithm, subjectPublicKey BIT STRING } */
    if (!asn1_expect(p, remaining, ASN1_TAG_SEQUENCE, &t)) return 0;
    {
        asn1_tlv_t spki = t;
        asn1_tlv_t alg, alg_oid;
        if (!asn1_expect(spki.value, spki.len, ASN1_TAG_SEQUENCE, &alg)) return 0;
        if (!asn1_parse_tlv(alg.value, alg.len, &alg_oid)) return 0;

        if (asn1_oid_equals(&alg_oid, OID_RSA_ENCRYPTION, sizeof(OID_RSA_ENCRYPTION))) {
            asn1_tlv_t pk_bits;
            u32 spki_remaining = spki.len - (u32)(alg.next - spki.value);
            if (asn1_expect(alg.next, spki_remaining, ASN1_TAG_BIT_STRING, &pk_bits)) {
                const u8 *rsa_data; u32 rsa_len;
                asn1_bit_string_bytes(&pk_bits, &rsa_data, &rsa_len);
                asn1_tlv_t rsa_seq, mod_t, exp_t;
                if (asn1_expect(rsa_data, rsa_len, ASN1_TAG_SEQUENCE, &rsa_seq) &&
                    asn1_expect(rsa_seq.value, rsa_seq.len, ASN1_TAG_INTEGER, &mod_t)) {
                    u32 mr = rsa_seq.len - (u32)(mod_t.next - rsa_seq.value);
                    if (asn1_expect(mod_t.next, mr, ASN1_TAG_INTEGER, &exp_t)) {
                        /* strip a possible leading 0x00 sign-padding byte
                         * before loading into the bignum, so a modulus
                         * whose top bit is set (needing that pad byte to
                         * stay a positive DER INTEGER) loads as exactly
                         * its real bit length, not 8 bits wider */
                        const u8 *mv = mod_t.value; u32 ml = mod_t.len;
                        if (ml > 0 && mv[0] == 0x00) { mv++; ml--; }
                        bn_from_bytes_be(&cert->pubkey_modulus, mv, ml);
                        cert->pubkey_exponent = asn1_small_int(&exp_t);
                        cert->pubkey_valid = 1;
                    }
                }
            }
        }
    }
    remaining -= (u32)(t.next - p); p = t.next;

    /* issuerUniqueID, subjectUniqueID: optional, essentially unused in
     * the wild (a v2-era feature). Anything left in this loop that
     * isn't extensions' [3] wrapper is simply skipped by length --
     * everything this client needs has already been captured above. */
    while (remaining > 0) {
        if (!asn1_parse_tlv(p, remaining, &t)) break;
        if (t.tag == ASN1_TAG_CONTEXT(3)) {
            asn1_tlv_t ext_seq;
            if (asn1_expect(t.value, t.len, ASN1_TAG_SEQUENCE, &ext_seq)) {
                x509_parse_extensions(cert, ext_seq.value, ext_seq.len);
            }
        }
        remaining -= (u32)(t.next - p);
        p = t.next;
    }

    /* Back at the outer Certificate level for signatureAlgorithm +
     * signatureValue -- siblings of tbsCertificate, not inside it. */
    u32 outer_remaining = outer.len - (u32)(tbs.next - outer.value);
    const u8 *op = tbs.next;

    asn1_tlv_t sig_alg, sig_alg_oid;
    if (!asn1_expect(op, outer_remaining, ASN1_TAG_SEQUENCE, &sig_alg)) return 0;
    if (!asn1_parse_tlv(sig_alg.value, sig_alg.len, &sig_alg_oid)) return 0;
    cert->sig_alg_supported = asn1_oid_equals(&sig_alg_oid, OID_SHA256_WITH_RSA, sizeof(OID_SHA256_WITH_RSA));
    outer_remaining -= (u32)(sig_alg.next - op);
    op = sig_alg.next;

    asn1_tlv_t sig_bits;
    if (!asn1_expect(op, outer_remaining, ASN1_TAG_BIT_STRING, &sig_bits)) return 0;
    {
        const u8 *sd; u32 sl;
        asn1_bit_string_bytes(&sig_bits, &sd, &sl);
        u32 n = sl < sizeof(cert->signature) ? sl : sizeof(cert->signature);
        for (u32 i = 0; i < n; i++) cert->signature[i] = sd[i];
        cert->signature_len = n;
    }

    return 1;
}

/* Verifies that `cert` was signed by whoever holds the private key
 * matching (issuer_modulus, issuer_exponent) -- the actual "is this
 * link in the chain legitimate" check. Steps: raise the signature to
 * the issuer's public exponent mod their modulus (the RSA "public"
 * operation -- verification is always the cheap direction, no private
 * key needed on either side of this call), which should recover a
 * PKCS#1 v1.5 padded block; check the padding shape (0x00 0x01, a run
 * of 0xFF bytes, a 0x00 terminator); check what follows is exactly the
 * SHA-256 DigestInfo prefix; and check the 32 bytes after THAT equal
 * SHA-256(cert->tbs_data). Every one of those checks has to pass --
 * this function is the one place in the whole TLS client where a
 * single missed check turns "verified" into decorative theater, so
 * each step returns 0 immediately rather than falling through. */
/* Verifies that `cert` was signed by whoever holds the private key
 * matching (issuer_modulus, issuer_exponent) -- the actual "is this
 * link in the chain legitimate" check. Delegates the RSA-PKCS1v1.5-SHA256
 * unwrap-and-compare to pkcs1.h's pkcs1_verify_sha256(), applied to
 * exactly the tbsCertificate bytes this certificate claims were
 * signed. */
static inline int x509_verify_signature(const x509_cert_t *cert, const bignum_t *issuer_modulus, u32 issuer_exponent) {
    if (!cert->sig_alg_supported) return 0; /* not SHA-256+RSA -- we cannot check this */
    return pkcs1_verify_sha256(cert->tbs_data, cert->tbs_len,
                                cert->signature, cert->signature_len,
                                issuer_modulus, issuer_exponent);
}

static inline int x509_is_valid_now(const x509_cert_t *cert, u64 now_packed) {
    return now_packed >= cert->not_before && now_packed <= cert->not_after;
}

static inline int x509_ascii_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Case-insensitive equality, with ONE special case: if `pattern` starts
 * with "*." it matches any `host` that has exactly one additional label
 * in place of the star (RFC 6125's rule -- "*.example.com" matches
 * "www.example.com" but not "example.com" itself or
 * "a.b.example.com"). Every real CA-issued wildcard certificate in
 * practice uses exactly this single-leftmost-label form, so that's the
 * only wildcard shape this function bothers recognizing. */
static inline int x509_name_matches_host(const u8 *pattern, u32 pattern_len, const char *host) {
    u32 host_len = 0;
    while (host[host_len]) host_len++;

    if (pattern_len > 2 && pattern[0] == '*' && pattern[1] == '.') {
        /* find host's first '.' -- everything after it must equal
         * pattern's bytes after "*." exactly (case-insensitively) */
        u32 dot = 0;
        while (dot < host_len && host[dot] != '.') dot++;
        if (dot == host_len) return 0; /* host has no dot at all -- can't match a wildcard */
        u32 host_suffix_len = host_len - dot - 1;
        u32 pattern_suffix_len = pattern_len - 2;
        if (host_suffix_len != pattern_suffix_len) return 0;
        for (u32 i = 0; i < pattern_suffix_len; i++) {
            if (x509_ascii_lower(pattern[2 + i]) != x509_ascii_lower(host[dot + 1 + i])) return 0;
        }
        return 1;
    }

    if (pattern_len != host_len) return 0;
    for (u32 i = 0; i < host_len; i++) {
        if (x509_ascii_lower(pattern[i]) != x509_ascii_lower((u8)host[i])) return 0;
    }
    return 1;
}

/* Checks whether `cert` is valid for `hostname` -- walks
 * subjectAltName's dNSName entries if the extension is present (the
 * only place a modern, CA/Browser-Forum-compliant certificate is
 * supposed to carry hostnames), and falls back to the Subject's
 * commonName only when there's no SAN extension at all, per RFC 6125's
 * grudging allowance for that legacy case. A cert with a SAN extension
 * that simply doesn't list this hostname is correctly rejected even if
 * its CN happens to match -- modern verification doesn't consult CN
 * once SAN exists, and neither does this function. */
static inline int x509_matches_hostname(const x509_cert_t *cert, const char *hostname) {
    if (cert->san_ext_value) {
        /* SubjectAltName ::= GeneralNames ::= SEQUENCE OF GeneralName;
         * GeneralName ::= CHOICE { ..., dNSName [2] IA5String, ... } --
         * so within the extnValue OCTET STRING's own content, we expect
         * one more SEQUENCE wrapper, then a run of context-specific
         * tagged TLVs, of which we only care about tag 2 (dNSName). */
        asn1_tlv_t names_seq;
        if (!asn1_expect(cert->san_ext_value, cert->san_ext_len, ASN1_TAG_SEQUENCE, &names_seq)) return 0;
        const u8 *p = names_seq.value;
        u32 remaining = names_seq.len;
        int found_any_dns_name = 0;
        while (remaining > 0) {
            asn1_tlv_t gn;
            if (!asn1_parse_tlv(p, remaining, &gn)) break;
            if (gn.tag == ASN1_TAG_CONTEXT_PRIMITIVE(2)) { /* dNSName, IMPLICIT primitive [2] --
                                                            * see asn1.h's ASN1_TAG_CONTEXT_PRIMITIVE
                                                            * for why this is 0x82 and not 0xA2 */
                found_any_dns_name = 1;
                if (x509_name_matches_host(gn.value, gn.len, hostname)) return 1;
            }
            remaining -= (u32)(gn.next - p);
            p = gn.next;
        }
        /* SAN extension present but had no dNSName entries at all (an
         * email-only or IP-only cert, say) -- per RFC 6125, still don't
         * fall back to CN in this case; a cert that deliberately lists
         * no DNS names is not claiming to be valid for any hostname. */
        (void)found_any_dns_name;
        return 0;
    }

    if (cert->cn_len > 0) {
        return x509_name_matches_host(cert->cn, cert->cn_len, hostname);
    }
    return 0;
}

/* Byte-for-byte comparison of one cert's issuer Name against another's
 * subject Name -- the link that lets a chain be walked: "cert A's
 * issuer is exactly cert B's subject" is what justifies even attempting
 * to verify A's signature against B's public key in the first place. */
static inline int x509_issuer_matches_subject(const x509_cert_t *child, const x509_cert_t *parent) {
    if (child->issuer_len != parent->subject_len) return 0;
    for (u32 i = 0; i < child->issuer_len; i++) {
        if (child->issuer[i] != parent->subject[i]) return 0;
    }
    return 1;
}

#endif
