#ifndef ASN1_H
#define ASN1_H
#include "io.h"

/* ============================================================
 * asn1.h -- a minimal DER (Distinguished Encoding Rules) reader. DER is
 * ASN.1's "there is exactly one correct way to encode this value"
 * subset -- the format every X.509 certificate on Earth is written in,
 * precisely because its rigidity is what makes signature verification
 * possible at all (BER's many equally-valid encodings of the same
 * value would make "hash these exact bytes" ambiguous).
 *
 * This is a reader, not a writer, and not a general ASN.1 library: it
 * knows how to walk a TLV (Tag-Length-Value) structure and decode the
 * handful of types X.509 actually uses (INTEGER, SEQUENCE, SET, OID,
 * BIT STRING, OCTET STRING, a couple of string types, and the two time
 * formats). Anything else -- an extension this code doesn't recognize,
 * a field it doesn't care about -- gets skipped by its own declared
 * length rather than requiring this parser to understand every corner
 * of X.509 to safely walk past the parts it doesn't need. That's the
 * whole trick DER's TLV structure buys you: you can always skip what
 * you don't understand, because the length is always right there.
 * ============================================================ */

#define ASN1_TAG_INTEGER          0x02
#define ASN1_TAG_BIT_STRING       0x03
#define ASN1_TAG_OCTET_STRING     0x04
#define ASN1_TAG_NULL             0x05
#define ASN1_TAG_OID              0x06
#define ASN1_TAG_UTF8_STRING      0x0C
#define ASN1_TAG_PRINTABLE_STRING 0x13
#define ASN1_TAG_IA5_STRING       0x16
#define ASN1_TAG_UTC_TIME         0x17
#define ASN1_TAG_GENERALIZED_TIME 0x18
#define ASN1_TAG_SEQUENCE         0x30  /* 0x10 | constructed bit */
#define ASN1_TAG_SET              0x31  /* 0x11 | constructed bit */
/* Context-specific constructed tags [0], [1], [2], [3] -- X.509 uses
 * these for EXPLICITLY-tagged optional fields like extensions ([3]) and
 * the version field ([0]); the raw tag byte is 0xA0 + the tag number
 * (0xA0 = 0x20 constructed-bit | 0x80 context-class-bit). */
#define ASN1_TAG_CONTEXT(n) (u8)(0xA0 + (n))
/* Context-specific PRIMITIVE tags -- distinct from the constructed form
 * above, and easy to get backwards: whether an IMPLICIT-tagged field
 * comes out as 0xA0+n (constructed) or 0x80+n (primitive) depends on
 * whether the underlying type it replaces was itself constructed. An
 * IA5String (like GeneralName's dNSName, [2] IMPLICIT IA5String) is a
 * primitive type, so IMPLICIT tagging it keeps the primitive bit clear:
 * tag byte 0x80+n, NOT 0xA0+n. Getting this wrong doesn't crash
 * anything -- it just silently fails to recognize any dNSName entry at
 * all, which is exactly the kind of quiet failure that's worth a
 * comment this pointed. */
#define ASN1_TAG_CONTEXT_PRIMITIVE(n) (u8)(0x80 + (n))

typedef struct {
    u8  tag;
    u32 len;
    const u8 *value;   /* points directly into the original buffer --
                        * this parser never copies, only slices, so
                        * every asn1_tlv_t's lifetime is tied to
                        * whatever buffer it was parsed from */
    const u8 *next;    /* where the next TLV (a sibling, at the same
                        * nesting level) starts -- value + len, computed
                        * once here so callers walking a SEQUENCE don't
                        * all have to redo that arithmetic themselves */
} asn1_tlv_t;

/* Parses one TLV starting at `data` (with `max_len` bytes available,
 * so this never reads past the end of whatever buffer it was handed).
 * Returns 1 on success, 0 on a malformed/truncated encoding -- checked
 * by every caller, since a hostile or corrupted certificate is exactly
 * the kind of input this function has to not crash on. */
static inline int asn1_parse_tlv(const u8 *data, u32 max_len, asn1_tlv_t *out) {
    if (max_len < 2) return 0;
    out->tag = data[0];
    u32 pos = 1;

    u8 len_byte = data[pos++];
    u32 len;
    if (len_byte < 0x80) {
        /* short form: the length IS this byte */
        len = len_byte;
    } else {
        /* long form: low 7 bits of len_byte say how many following
         * bytes encode the actual length, big-endian */
        u8 num_len_bytes = (u8)(len_byte & 0x7F);
        if (num_len_bytes == 0 || num_len_bytes > 4) return 0; /* 0 = indefinite
                                                                 * length, which DER
                                                                 * (unlike BER)
                                                                 * never uses -- and
                                                                 * >4 bytes of length
                                                                 * would describe
                                                                 * something bigger
                                                                 * than any cert this
                                                                 * client will ever
                                                                 * see, so treat it as
                                                                 * malformed rather
                                                                 * than try to
                                                                 * represent it */
        if (pos + num_len_bytes > max_len) return 0;
        len = 0;
        for (u8 i = 0; i < num_len_bytes; i++) len = (len << 8) | data[pos++];
    }

    if (pos + len > max_len) return 0; /* declared length runs past what we actually have */

    out->len = len;
    out->value = data + pos;
    out->next = data + pos + len;
    return 1;
}

/* Convenience for the extremely common case of "this TLV should be a
 * SEQUENCE (or SET), now give me a cursor to walk its children" --
 * just asn1_parse_tlv() with a tag check, since SEQUENCE/SET's own
 * *contents* are simply back-to-back TLVs with no extra framing beyond
 * what the outer TLV's length already told you. */
static inline int asn1_expect(const u8 *data, u32 max_len, u8 expect_tag, asn1_tlv_t *out) {
    if (!asn1_parse_tlv(data, max_len, out)) return 0;
    return out->tag == expect_tag;
}

/* Decodes an INTEGER TLV's value into a plain u32 -- only meaningful
 * for small integers (version numbers, and similar fields this client
 * actually branches on); RSA moduli and signatures are also tagged
 * INTEGER but are handled separately via bn_from_bytes_be() directly on
 * the TLV's raw value bytes, not through this function. DER INTEGERs
 * are signed and minimally encoded (a leading 0x00 byte is present only
 * when needed to keep a value whose top bit is set from looking
 * negative) -- this function skips exactly one leading 0x00 if present
 * and otherwise reads the bytes as unsigned big-endian, which is
 * correct for every non-negative small integer X.509 actually uses
 * this for. */
static inline u32 asn1_small_int(const asn1_tlv_t *t) {
    const u8 *p = t->value;
    u32 len = t->len;
    if (len > 0 && p[0] == 0x00) { p++; len--; }
    u32 v = 0;
    for (u32 i = 0; i < len && i < 4; i++) v = (v << 8) | p[i];
    return v;
}

/* Compares an OID TLV's raw encoded bytes against a caller-provided
 * byte string -- the standard way to check "is this the OID for
 * sha256WithRSAEncryption" or similar, since OIDs are more naturally
 * compared as their DER byte encoding than decoded into dotted-decimal
 * and parsed back (nothing in this client ever needs to *print* an
 * OID, only recognize a small fixed set of them). */
static inline int asn1_oid_equals(const asn1_tlv_t *t, const u8 *oid_bytes, u32 oid_len) {
    if (t->tag != ASN1_TAG_OID) return 0;
    if (t->len != oid_len) return 0;
    for (u32 i = 0; i < oid_len; i++) if (t->value[i] != oid_bytes[i]) return 0;
    return 1;
}

/* BIT STRING encodings carry one extra leading byte (the count of
 * "unused bits" in the final octet -- always 0 for the byte-aligned
 * values X.509 uses it for: subjectPublicKey and the signature value).
 * This strips that leading byte and hands back a plain octet pointer +
 * length, which is what every actual caller wants. */
static inline void asn1_bit_string_bytes(const asn1_tlv_t *t, const u8 **out_data, u32 *out_len) {
    if (t->len == 0) { *out_data = t->value; *out_len = 0; return; }
    *out_data = t->value + 1;
    *out_len = t->len - 1;
}

#endif
