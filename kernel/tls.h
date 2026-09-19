#ifndef TLS_H
#define TLS_H
#include "io.h"
#include "sha256.h"
#include "hmac_sha256.h"
#include "aes.h"
#include "gcm.h"
#include "bignum.h"
#include "x25519.h"
#include "x509.h"
#include "pkcs1.h"
#include "trusted_roots.h"
#include "tcp.h"

/* ============================================================
 * tls.h -- TLS 1.2, one cipher suite only:
 * TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 (0xC02F). X25519 for key
 * exchange, RSA-PKCS1v1.5-SHA256 for authenticating the exchange (and
 * the certificate chain that backs it), AES-128-GCM for the record
 * cipher, SHA-256 for the handshake PRF.
 *
 * This is NOT the cipher suite this client started development with --
 * see the earlier design notes for TLS_RSA_WITH_AES_128_CBC_SHA256 (no
 * forward secrecy, RSA key transport instead of ECDHE). That suite was
 * abandoned after a real, security-conscious HTTPS endpoint flatly
 * rejected it with a handshake_failure alert during testing: static RSA
 * key exchange has fallen out of favor industry-wide for lacking
 * forward secrecy, and enough of the real web has dropped it that a
 * TLS client without ECDHE genuinely can't reach much of it anymore.
 * ECDHE_RSA with AES-GCM is the suite that actually works against real
 * servers today, so that's what got built, cipher-math complexity be
 * damned.
 *
 * Everything this client does NOT support -- and simply refuses,
 * rather than pretends to handle -- follows from that one cipher
 * suite: no TLS 1.3 (a different-enough protocol that "TLS 1.2 client,
 * pick one suite" doesn't extend to it for free), no ECDSA
 * certificates, no session resumption, no renegotiation, no client
 * certificates, no ALPN. A server that requires any of those fails
 * this connection cleanly (TLS_STATE_FAILED, a specific reason logged)
 * rather than silently misbehaving.
 * ============================================================ */

#define TLS_VERSION_1_2 0x0303

#define TLS_CONTENT_CHANGE_CIPHER_SPEC 20
#define TLS_CONTENT_ALERT              21
#define TLS_CONTENT_HANDSHAKE          22
#define TLS_CONTENT_APPLICATION_DATA   23

#define TLS_HS_HELLO_REQUEST        0
#define TLS_HS_CLIENT_HELLO         1
#define TLS_HS_SERVER_HELLO         2
#define TLS_HS_CERTIFICATE          11
#define TLS_HS_SERVER_KEY_EXCHANGE  12
#define TLS_HS_CERTIFICATE_REQUEST  13
#define TLS_HS_SERVER_HELLO_DONE    14
#define TLS_HS_CERTIFICATE_VERIFY   15
#define TLS_HS_CLIENT_KEY_EXCHANGE  16
#define TLS_HS_FINISHED             20

#define TLS_CIPHER_SUITE 0xC02F   /* TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 */

#define TLS_MAX_RECORD_PLAINTEXT 16384   /* TLS spec's own per-record cap */
/* rx_raw MUST be able to hold one complete record even at the maximum
 * legal size -- 5-byte header + 8-byte explicit nonce + up to 16384
 * bytes of plaintext + 16-byte GCM tag = 16413 bytes worst case. A
 * buffer smaller than that isn't just slow for large responses, it's
 * an outright deadlock: tls_process_raw_buffer() correctly waits for a
 * record's declared length to fully arrive before processing it (this
 * is *correct* stream-reassembly behavior, not a bug), but if that
 * length can never fit in the buffer at all, "wait for more data" never
 * resolves -- discovered exactly this way, testing against a real
 * server that (reasonably) sent its response as one large record. Sized
 * with a little headroom above the bare minimum for whatever of the
 * next record's bytes TCP happens to deliver in the same read. */
#define TLS_RX_RAW_BUF_SIZE   17408   /* 17KB: 16413-byte max record + headroom */
#define TLS_HS_BUF_SIZE       8192    /* reassembled handshake-content bytes,
                                       * not yet split into messages -- real
                                       * certificate chains (a leaf plus one
                                       * or two intermediates) run a few KB;
                                       * this has comfortable headroom above
                                       * anything actually observed */
#define TLS_APP_RECV_BUF_SIZE 4096   /* NOT sized for a worst-case 16384-byte
                                      * record -- see tls_process_raw_buffer()'s
                                      * application_data handling, which
                                      * truncates (keeps the first
                                      * TLS_APP_RECV_BUF_SIZE bytes, discards
                                      * the rest) rather than treating an
                                      * oversized response as fatal. This
                                      * matches https.h's own response
                                      * buffer size, which matches http.h's
                                      * -- this client only ever previews a
                                      * page's opening bytes, so keeping
                                      * app_recv sized for that instead of
                                      * for "the biggest single record TLS
                                      * legally allows" saves real BSS
                                      * budget for a limit this client
                                      * already imposes one layer up anyway. */
#define TLS_CHAIN_MAX 3              /* leaf + one or two intermediates/root.
                                      * Real chains are overwhelmingly 2-3
                                      * certificates (a server sending
                                      * leaf+intermediate and trusting the
                                      * client already has the root is the
                                      * common case; leaf+intermediate+root
                                      * explicitly, as this client's own
                                      * test network happened to send, is
                                      * the other common case) -- a server
                                      * sending a longer chain than that
                                      * fails cleanly here rather than
                                      * following it. */

typedef enum {
    TLS_CLOSED = 0,
    TLS_WAIT_TCP,             /* underlying tcp_connect() not ESTABLISHED yet */
    TLS_CLIENT_HELLO_SENT,    /* waiting for ServerHello..ServerHelloDone */
    TLS_CLIENT_FINISHED_SENT, /* waiting for server's ChangeCipherSpec+Finished */
    TLS_ESTABLISHED,
    TLS_FAILED,
} tls_state_t;

/* Why a handshake failed -- kept as plain reason codes rather than only
 * a serial-log line, so a caller (kernel.c's MiniWeb, eventually) can
 * show something more specific than "it didn't work." */
typedef enum {
    TLS_FAIL_NONE = 0,
    TLS_FAIL_TCP,
    TLS_FAIL_UNEXPECTED_MESSAGE,
    TLS_FAIL_UNSUPPORTED_CIPHER_SUITE,   /* server picked something other
                                          * than the one suite we offered --
                                          * shouldn't happen since we only
                                          * offer one, but a server that
                                          * echoes back nonsense gets
                                          * caught here rather than trusted */
    TLS_FAIL_UNSUPPORTED_CURVE,          /* ServerKeyExchange named a curve
                                          * other than x25519 */
    TLS_FAIL_CERT_PARSE,
    TLS_FAIL_CERT_CHAIN,                 /* a link in the chain didn't
                                          * verify (wrong issuer, bad
                                          * signature, expired, ...) */
    TLS_FAIL_CERT_UNTRUSTED,             /* chain is internally consistent
                                          * but doesn't reach any of our
                                          * embedded trusted roots */
    TLS_FAIL_HOSTNAME_MISMATCH,
    TLS_FAIL_SKE_SIGNATURE,              /* ServerKeyExchange's signature
                                          * didn't verify against the leaf
                                          * cert's key */
    TLS_FAIL_DECRYPT,                    /* a GCM tag check failed --
                                          * treated as tampering, full stop */
    TLS_FAIL_SERVER_FINISHED,            /* server's Finished verify_data
                                          * didn't match what we computed */
    TLS_FAIL_BUFFER_OVERFLOW,            /* a message or chain was bigger
                                          * than this client's fixed buffers --
                                          * refuses to overrun them rather
                                          * than silently truncate */
} tls_fail_reason_t;

typedef struct {
    tls_state_t state;
    tls_fail_reason_t fail_reason;

    char hostname[128];

    u8 client_random[32];
    u8 server_random[32];

    u8 my_private[32];   /* our ephemeral X25519 private key for this connection */
    u8 my_public[32];
    u8 peer_public[32];  /* server's ephemeral X25519 public key, from ServerKeyExchange */

    u8 master_secret[48];

    aes128_ctx_t client_write_aes, server_write_aes;
    u8 client_write_iv[4], server_write_iv[4];   /* the GCM "salt" half of
                                                  * each direction's nonce */
    u64 client_seq, server_seq;
    int write_cipher_active, read_cipher_active;

    sha256_ctx_t transcript;   /* running hash over every handshake message,
                               * sent or received, in order -- see
                               * tls_transcript_update() */

    x509_cert_t chain[TLS_CHAIN_MAX];
    int chain_len;

    u8 rx_raw[TLS_RX_RAW_BUF_SIZE];
    u32 rx_raw_len;
    u8 hs_buf[TLS_HS_BUF_SIZE];
    u32 hs_buf_len;
    u8 app_recv[TLS_APP_RECV_BUF_SIZE];
    u32 app_recv_len;

    /* A tiny xorshift-like PRNG state, seeded once by whatever entropy
     * kernel/tls.h's caller provides (see tls_connect()'s seed
     * parameter) -- used only for ClientHello's client_random and our
     * ephemeral X25519 private key (this cipher suite needs no PKCS#1
     * *encryption* padding at all -- ECDHE carries the secret, not RSA).
     * Not a hardened CSPRNG -- see tls_connect()'s own comment for the
     * honest caveat on that. */
    u32 rng_state[4];
} tls_conn_t;

static tls_conn_t tls_conn;

/* ---- a small non-cryptographic PRNG, xorshift128, seeded from
 * whatever entropy the caller has (RTC, tick counters, ...) ----
 * Explicitly NOT a cryptographically secure RNG: xorshift is fast,
 * simple, and has excellent statistical properties, but its internal
 * state is trivially invertible from a handful of outputs, which is
 * disqualifying for anything defending against an adversary who can
 * observe outputs and wants to predict future ones. For this client's
 * two actual uses -- ClientHello's client_random (public anyway, sent
 * in the clear) and an ephemeral X25519 private key (needs
 * unpredictability, which is the one place this matters) -- a hobby OS
 * with no hardware entropy source and no intention of defending against
 * a nation-state adversary is choosing "honestly weak and documented"
 * over "pretend to be strong and be wrong about it." A future session
 * wiring up RDRAND (checked via CPUID, a single available-on-any-
 * modern-x86 instruction) as the seed source, or as a direct
 * replacement when present, would meaningfully improve this without
 * needing a different algorithm here. */
static inline void tls_rng_seed(const u8 seed[16]) {
    for (int i = 0; i < 4; i++) {
        tls_conn.rng_state[i] = ((u32)seed[i*4] << 24) | ((u32)seed[i*4+1] << 16)
                               | ((u32)seed[i*4+2] << 8) | (u32)seed[i*4+3];
        if (tls_conn.rng_state[i] == 0) tls_conn.rng_state[i] = 0x9E3779B9u ^ (u32)i; /* never all-zero */
    }
}
static inline u32 tls_rng_u32(void) {
    u32 x = tls_conn.rng_state[0];
    u32 t = tls_conn.rng_state[3];
    tls_conn.rng_state[3] = tls_conn.rng_state[2];
    tls_conn.rng_state[2] = tls_conn.rng_state[1];
    tls_conn.rng_state[1] = x;
    t ^= t << 11; t ^= t >> 8;
    tls_conn.rng_state[0] = t ^ x ^ (x >> 19);
    return tls_conn.rng_state[0];
}
static inline void tls_rng_bytes(u8 *out, u32 len) {
    u32 i = 0;
    while (i < len) {
        u32 r = tls_rng_u32();
        for (int b = 0; b < 4 && i < len; b++, i++) out[i] = (u8)(r >> (b * 8));
    }
}

/* ---- TLS 1.2's PRF: P_SHA256(secret, label||seed), per RFC 5246 5 ----
 * A(0) = seed; A(i) = HMAC(secret, A(i-1))
 * P_hash = HMAC(secret, A(1)||seed) || HMAC(secret, A(2)||seed) || ...
 * truncated to out_len bytes. Every session key this connection ever
 * uses (master secret, then the whole key_block) comes out of this one
 * function, just with different (secret, label, seed, out_len). */
static inline void tls_prf(u8 *out, u32 out_len,
                            const u8 *secret, u32 secret_len,
                            const char *label,
                            const u8 *seed, u32 seed_len) {
    u8 label_seed[256];
    u32 label_len = 0;
    while (label[label_len]) label_len++;
    u32 ls_len = label_len + seed_len;
    for (u32 i = 0; i < label_len; i++) label_seed[i] = (u8)label[i];
    for (u32 i = 0; i < seed_len; i++) label_seed[label_len + i] = seed[i];

    u8 a[32];
    hmac_sha256(secret, secret_len, label_seed, ls_len, a); /* A(1) */

    u32 produced = 0;
    while (produced < out_len) {
        u8 a_plus_seed[32 + 256];
        for (int i = 0; i < 32; i++) a_plus_seed[i] = a[i];
        for (u32 i = 0; i < ls_len; i++) a_plus_seed[32 + i] = label_seed[i];

        u8 block[32];
        hmac_sha256(secret, secret_len, a_plus_seed, 32 + ls_len, block);

        u32 take = out_len - produced;
        if (take > 32) take = 32;
        for (u32 i = 0; i < take; i++) out[produced + i] = block[i];
        produced += take;

        u8 next_a[32];
        hmac_sha256(secret, secret_len, a, 32, next_a);
        for (int i = 0; i < 32; i++) a[i] = next_a[i];
    }
}

/* Feeds one complete handshake message's raw bytes (4-byte type+length
 * header, then body) into the running transcript hash. Called exactly
 * once per message, at the point it's sent or fully received/dispatched
 * -- see the file header for why message order here has to exactly
 * match both sides' actual send/receive order. */
static inline void tls_transcript_update(const u8 *msg, u32 msg_len) {
#ifdef TLS_DEBUG_TRANSCRIPT
    fprintf(stderr, "[transcript] type=%d len=%u bytes=", msg[0], msg_len);
    for (u32 i = 0; i < msg_len; i++) fprintf(stderr, "%02x", msg[i]);
    fprintf(stderr, "\n");
#endif
    sha256_update(&tls_conn.transcript, msg, msg_len);
}

/* Writes one TLS record: header (content type, version, length) plus
 * payload, encrypting the payload first if write_cipher_active. This is
 * the ONLY function in this file that calls tcp_send_data() -- every
 * handshake message and every byte of application data funnels through
 * here, so the encryption decision lives in exactly one place. */
static inline int tls_write_record(u8 content_type, const u8 *plaintext, u32 len) {
    u8 record[5 + TLS_MAX_RECORD_PLAINTEXT + 8 + 16];
    u32 payload_len;

    if (tls_conn.write_cipher_active) {
        u8 nonce[12];
        for (int i = 0; i < 4; i++) nonce[i] = tls_conn.client_write_iv[i];
        for (int i = 0; i < 8; i++) nonce[4 + i] = (u8)(tls_conn.client_seq >> (56 - i * 8));

        u8 aad[13];
        for (int i = 0; i < 8; i++) aad[i] = (u8)(tls_conn.client_seq >> (56 - i * 8));
        aad[8] = content_type;
        aad[9] = (u8)(TLS_VERSION_1_2 >> 8);
        aad[10] = (u8)(TLS_VERSION_1_2 & 0xFF);
        aad[11] = (u8)(len >> 8);
        aad[12] = (u8)(len & 0xFF);

        u8 ciphertext[TLS_MAX_RECORD_PLAINTEXT];
        for (u32 i = 0; i < len; i++) ciphertext[i] = plaintext[i];
        u8 tag[16];
        gcm_encrypt(&tls_conn.client_write_aes, nonce, aad, 13, ciphertext, len, tag);

        for (int i = 0; i < 8; i++) record[5 + i] = nonce[4 + i]; /* explicit nonce = seq num */
        for (u32 i = 0; i < len; i++) record[5 + 8 + i] = ciphertext[i];
        for (int i = 0; i < 16; i++) record[5 + 8 + len + i] = tag[i];
        payload_len = 8 + len + 16;
        tls_conn.client_seq++;
    } else {
        for (u32 i = 0; i < len; i++) record[5 + i] = plaintext[i];
        payload_len = len;
    }

    record[0] = content_type;
    record[1] = (u8)(TLS_VERSION_1_2 >> 8);
    record[2] = (u8)(TLS_VERSION_1_2 & 0xFF);
    record[3] = (u8)(payload_len >> 8);
    record[4] = (u8)(payload_len & 0xFF);

    return tcp_send_data(record, (u16)(5 + payload_len));
}

/* Sends one handshake message: builds the 4-byte type+length header,
 * writes header+body as one record (tls_write_record handles
 * encryption once that's active), and folds the same bytes into the
 * transcript hash. Every handshake message this client sends
 * (ClientHello, ClientKeyExchange, Finished) goes through this one
 * function so those two things can never accidentally drift apart. */
static inline int tls_send_handshake(u8 msg_type, const u8 *body, u32 body_len) {
    u8 msg[4 + 4096];
    msg[0] = msg_type;
    msg[1] = (u8)(body_len >> 16);
    msg[2] = (u8)(body_len >> 8);
    msg[3] = (u8)(body_len & 0xFF);
    for (u32 i = 0; i < body_len; i++) msg[4 + i] = body[i];

    tls_transcript_update(msg, 4 + body_len);
    return tls_write_record(TLS_CONTENT_HANDSHAKE, msg, 4 + body_len);
}

/* Builds and sends ClientHello: version, client_random, empty session
 * ID, our one cipher suite, null compression, and the three extensions
 * a modern ECDHE_RSA handshake actually needs (SNI so the server picks
 * the right certificate; supported_groups=x25519 and
 * ec_point_formats=uncompressed so the server knows we can do the key
 * exchange we're about to demand; signature_algorithms so it knows
 * rsa_pkcs1_sha256 is an acceptable way to sign ServerKeyExchange). */
static inline void tls_send_client_hello(void) {
    tls_rng_bytes(tls_conn.client_random, 32);

    u32 host_len = 0;
    while (tls_conn.hostname[host_len]) host_len++;

    u8 body[512];
    u32 pos = 0;
    body[pos++] = (u8)(TLS_VERSION_1_2 >> 8);
    body[pos++] = (u8)(TLS_VERSION_1_2 & 0xFF);
    for (int i = 0; i < 32; i++) body[pos++] = tls_conn.client_random[i];
    body[pos++] = 0; /* session_id length: 0, no resumption offered */

    body[pos++] = 0x00; body[pos++] = 0x02; /* cipher_suites length: 2 bytes = 1 suite */
    body[pos++] = (u8)(TLS_CIPHER_SUITE >> 8);
    body[pos++] = (u8)(TLS_CIPHER_SUITE & 0xFF);

    body[pos++] = 0x01; body[pos++] = 0x00; /* compression_methods: 1 method, null (0) */

    u32 ext_len_pos = pos; pos += 2; /* extensions length, filled in after */
    u32 ext_start = pos;

    /* server_name (SNI): extension_type=0, list of {name_type=0(host_name), name} */
    {
        body[pos++] = 0x00; body[pos++] = 0x00;
        u32 ext_body_len_pos = pos; pos += 2;
        u32 list_len_pos = pos; pos += 2;
        body[pos++] = 0x00; /* name_type: host_name */
        body[pos++] = (u8)(host_len >> 8); body[pos++] = (u8)(host_len & 0xFF);
        for (u32 i = 0; i < host_len; i++) body[pos++] = (u8)tls_conn.hostname[i];
        u32 list_len = pos - list_len_pos - 2;
        body[list_len_pos] = (u8)(list_len >> 8); body[list_len_pos+1] = (u8)(list_len & 0xFF);
        u32 ext_body_len = pos - ext_body_len_pos - 2;
        body[ext_body_len_pos] = (u8)(ext_body_len >> 8); body[ext_body_len_pos+1] = (u8)(ext_body_len & 0xFF);
    }
    /* supported_groups: extension_type=10, list of named groups -- x25519 (0x001D) only */
    {
        body[pos++] = 0x00; body[pos++] = 0x0A;
        body[pos++] = 0x00; body[pos++] = 0x04; /* ext body len */
        body[pos++] = 0x00; body[pos++] = 0x02; /* list len */
        body[pos++] = 0x00; body[pos++] = 0x1D; /* x25519 */
    }
    /* ec_point_formats: extension_type=11 -- uncompressed(0) only.
     * Not actually meaningful for X25519 (which has no point-compression
     * concept the way NIST curves do), but servers/middleboxes from the
     * ECDHE-over-NIST-curves era sometimes still expect this extension
     * to be present at all, so it's included for compatibility. */
    {
        body[pos++] = 0x00; body[pos++] = 0x0B;
        body[pos++] = 0x00; body[pos++] = 0x02;
        body[pos++] = 0x01; body[pos++] = 0x00; /* list len=1, format=uncompressed */
    }
    /* signature_algorithms: extension_type=13 -- rsa_pkcs1_sha256 (0x0401) only */
    {
        body[pos++] = 0x00; body[pos++] = 0x0D;
        body[pos++] = 0x00; body[pos++] = 0x04;
        body[pos++] = 0x00; body[pos++] = 0x02;
        body[pos++] = 0x04; body[pos++] = 0x01;
    }

    u32 ext_total_len = pos - ext_start;
    body[ext_len_pos] = (u8)(ext_total_len >> 8);
    body[ext_len_pos+1] = (u8)(ext_total_len & 0xFF);

    tls_send_handshake(TLS_HS_CLIENT_HELLO, body, pos);
}

/* Parses ServerHello's body (already stripped of its 4-byte handshake
 * header by the caller). Only reads what this client needs: version
 * (logged but not enforced beyond "did the server even try TLS 1.2"),
 * server_random, and the cipher suite (which MUST echo the one suite we
 * offered, or this connection fails rather than silently accepting a
 * server picking something we never offered). Session ID and any
 * extensions are present but unread -- this client sent no extensions
 * whose response it needs to act on (no ALPN, no session tickets). */
static inline int tls_parse_server_hello(const u8 *body, u32 len) {
    if (len < 2 + 32 + 1) return 0;
    u32 pos = 2; /* skip version */
    for (int i = 0; i < 32; i++) tls_conn.server_random[i] = body[pos + i];
    pos += 32;
    u8 session_id_len = body[pos++];
    pos += session_id_len;
    if (pos + 3 > len) return 0;
    u16 cipher_suite = (u16)((body[pos] << 8) | body[pos+1]);
    pos += 2;
    /* pos now at compression_method (1 byte), then extensions -- skip both, unread */
    (void)pos;

    if (cipher_suite != TLS_CIPHER_SUITE) {
        tls_conn.fail_reason = TLS_FAIL_UNSUPPORTED_CIPHER_SUITE;
        return 0;
    }
    return 1;
}

/* Parses the Certificate message body: a 3-byte total-length prefix
 * followed by a sequence of {3-byte cert length, cert DER bytes}.
 * Parses each into tls_conn.chain[] via x509_parse() -- cryptographic
 * verification of the chain happens separately, in
 * tls_verify_certificate_chain(), once every certificate is at least
 * syntactically parsed. */
static inline int tls_parse_certificate_message(const u8 *body, u32 len) {
    if (len < 3) return 0;
    u32 total_len = ((u32)body[0] << 16) | ((u32)body[1] << 8) | body[2];
    if (3 + total_len > len) return 0;

    u32 pos = 3;
    u32 end = 3 + total_len;
    int count = 0;
    while (pos < end) {
        if (pos + 3 > end) return 0;
        u32 cert_len = ((u32)body[pos] << 16) | ((u32)body[pos+1] << 8) | body[pos+2];
        pos += 3;
        if (pos + cert_len > end) return 0;
        if (count >= TLS_CHAIN_MAX) {
            tls_conn.fail_reason = TLS_FAIL_BUFFER_OVERFLOW;
            return 0;
        }
        if (!x509_parse(body + pos, cert_len, &tls_conn.chain[count])) {
            tls_conn.fail_reason = TLS_FAIL_CERT_PARSE;
            return 0;
        }
        count++;
        pos += cert_len;
    }
    tls_conn.chain_len = count;
    return count > 0;
}

/* Packs the current RTC-supplied date into the same YYYYMMDDHHMMSS u64
 * format x509.h's validity fields use. Takes the fields directly
 * (rather than an rtc_time_t, so this header doesn't need to depend on
 * kernel/rtc.h) -- kernel.c's integration code is what actually reads
 * the CMOS clock and calls this. */
static inline u64 tls_pack_datetime(u32 year, u32 month, u32 day, u32 hour, u32 minute, u32 second) {
    return (u64)year * 10000000000ull + (u64)month * 100000000ull + (u64)day * 1000000ull
         + (u64)hour * 10000ull + (u64)minute * 100ull + (u64)second;
}

/* Walks tls_conn.chain[] (as parsed by tls_parse_certificate_message())
 * and checks: each certificate's issuer matches the next one's subject,
 * each certificate's signature verifies against the next one's public
 * key, every certificate is within its validity window, the leaf
 * matches `hostname`, and finally that the chain's last certificate was
 * signed by one of this client's embedded trusted roots. That last
 * check alone covers both ways a server's chain can reach a trusted
 * root: sending leaf+intermediate only (the common case -- the last
 * cert in THAT chain is the intermediate, verified here against our own
 * copy of the root's public key) and sending the root explicitly too
 * (then the "last cert" IS the root itself, self-signed, and
 * "verify its signature against our embedded copy of the same root's
 * key" succeeds precisely because it's the same key signing itself). A
 * server whose chain doesn't reach any embedded root fails with
 * TLS_FAIL_CERT_UNTRUSTED -- this client never falls back to trusting
 * an unrecognized self-signed certificate just because the chain is
 * otherwise internally consistent (that would make any attacker's own
 * self-signed cert as good as a real one). */
static inline int tls_verify_certificate_chain(u64 now_packed) {
    if (tls_conn.chain_len == 0) { tls_conn.fail_reason = TLS_FAIL_CERT_CHAIN; return 0; }

    for (int i = 0; i < tls_conn.chain_len; i++) {
        x509_cert_t *cert = &tls_conn.chain[i];
        if (!cert->pubkey_valid || !cert->sig_alg_supported) {
            tls_conn.fail_reason = TLS_FAIL_CERT_CHAIN;
            return 0;
        }
        if (!x509_is_valid_now(cert, now_packed)) {
            tls_conn.fail_reason = TLS_FAIL_CERT_CHAIN;
            return 0;
        }
    }

    if (!x509_matches_hostname(&tls_conn.chain[0], tls_conn.hostname)) {
        tls_conn.fail_reason = TLS_FAIL_HOSTNAME_MISMATCH;
        return 0;
    }

    for (int i = 0; i < tls_conn.chain_len - 1; i++) {
        x509_cert_t *child = &tls_conn.chain[i];
        x509_cert_t *parent = &tls_conn.chain[i + 1];
        if (!x509_issuer_matches_subject(child, parent)) {
            tls_conn.fail_reason = TLS_FAIL_CERT_CHAIN;
            return 0;
        }
        if (!parent->is_ca) {
            tls_conn.fail_reason = TLS_FAIL_CERT_CHAIN;
            return 0;
        }
        if (!x509_verify_signature(child, &parent->pubkey_modulus, parent->pubkey_exponent)) {
            tls_conn.fail_reason = TLS_FAIL_CERT_CHAIN;
            return 0;
        }
    }

    x509_cert_t *last = &tls_conn.chain[tls_conn.chain_len - 1];
    for (int r = 0; r < TRUSTED_ROOT_COUNT; r++) {
        x509_cert_t root;
        if (!x509_parse(trusted_roots[r], trusted_root_lens[r], &root)) continue;
        if (x509_issuer_matches_subject(last, &root) &&
            x509_verify_signature(last, &root.pubkey_modulus, root.pubkey_exponent)) {
            return 1;
        }
    }

    tls_conn.fail_reason = TLS_FAIL_CERT_UNTRUSTED;
    return 0;
}

/* Parses ServerKeyExchange for the ECDHE_RSA case: ECParameters
 * (curve_type=named_curve(3), named_curve -- MUST be x25519/0x001D, or
 * this client can't do the exchange and fails cleanly), the server's
 * ephemeral public key (length-prefixed opaque), and a signature
 * (SignatureAndHashAlgorithm + length-prefixed signature bytes) over
 * (client_random || server_random || the ECParameters and public key
 * bytes exactly as they appeared on the wire). Verifies that signature
 * against the LEAF certificate's RSA public key -- this is what
 * actually proves the party we're key-exchanging with is the same one
 * that holds the certificate's private key, tying the cryptographic
 * channel to the identity the chain vouches for. Without this check, an
 * attacker with no certificate at all could still complete a key
 * exchange; this is the step that closes that gap. */
static inline int tls_parse_server_key_exchange(const u8 *body, u32 len) {
    if (len < 4) { tls_conn.fail_reason = TLS_FAIL_UNEXPECTED_MESSAGE; return 0; }
    if (body[0] != 3) { tls_conn.fail_reason = TLS_FAIL_UNSUPPORTED_CURVE; return 0; } /* curve_type: named_curve */
    u16 named_curve = (u16)((body[1] << 8) | body[2]);
    if (named_curve != 0x001D) { tls_conn.fail_reason = TLS_FAIL_UNSUPPORTED_CURVE; return 0; }
    u8 pubkey_len = body[3];
    if (pubkey_len != 32 || 4u + pubkey_len > len) { tls_conn.fail_reason = TLS_FAIL_UNSUPPORTED_CURVE; return 0; }

    u32 params_end = 4 + pubkey_len;
    for (int i = 0; i < 32; i++) tls_conn.peer_public[i] = body[4 + i];

    if (params_end + 4 > len) { tls_conn.fail_reason = TLS_FAIL_UNEXPECTED_MESSAGE; return 0; }
    /* SignatureAndHashAlgorithm: 2 bytes -- this client only ever asked
     * for rsa_pkcs1_sha256 (0x0401) in ClientHello, and only knows how
     * to verify that one, so anything else is a hard fail rather than
     * an attempt to also support whatever the server picked instead. */
    if (body[params_end] != 0x04 || body[params_end + 1] != 0x01) {
        tls_conn.fail_reason = TLS_FAIL_SKE_SIGNATURE;
        return 0;
    }
    u16 sig_len = (u16)((body[params_end + 2] << 8) | body[params_end + 3]);
    u32 sig_start = params_end + 4;
    if (sig_start + sig_len > len) { tls_conn.fail_reason = TLS_FAIL_UNEXPECTED_MESSAGE; return 0; }

    u8 signed_data[4 + 512];
    u32 sd_pos = 0;
    for (int i = 0; i < 32; i++) signed_data[sd_pos++] = tls_conn.client_random[i];
    for (int i = 0; i < 32; i++) signed_data[sd_pos++] = tls_conn.server_random[i];
    for (u32 i = 0; i < params_end; i++) signed_data[sd_pos++] = body[i];

    x509_cert_t *leaf = &tls_conn.chain[0];
    if (!pkcs1_verify_sha256(signed_data, sd_pos, body + sig_start, sig_len,
                              &leaf->pubkey_modulus, leaf->pubkey_exponent)) {
        tls_conn.fail_reason = TLS_FAIL_SKE_SIGNATURE;
        return 0;
    }
    return 1;
}

/* Builds and sends ClientKeyExchange: just our ephemeral X25519 public
 * key, length-prefixed. (The RSA-key-transport variant of this message
 * -- an encrypted premaster secret -- belongs to the cipher suite this
 * client abandoned; ECDHE's ClientKeyExchange is this much simpler,
 * one of the few places the pivot to ECDHE actually reduced code.) */
static inline void tls_send_client_key_exchange(void) {
    u8 body[1 + 32];
    body[0] = 32;
    for (int i = 0; i < 32; i++) body[1 + i] = tls_conn.my_public[i];
    tls_send_handshake(TLS_HS_CLIENT_KEY_EXCHANGE, body, sizeof(body));
}

/* Derives master_secret from the X25519 shared secret (the "premaster
 * secret" in TLS's terminology, same PRF-based derivation regardless of
 * which key exchange produced it) and, from master_secret, the full
 * key_block this cipher suite needs: client/server write keys (16 bytes
 * each) and client/server write IVs (4-byte GCM salts each) -- no MAC
 * keys, since GCM is AEAD and authenticates via the tag, not a separate
 * HMAC the way the CBC suites this client doesn't support would need. */
static inline void tls_derive_keys(const u8 premaster[32]) {
    u8 seed[64];
    for (int i = 0; i < 32; i++) seed[i] = tls_conn.client_random[i];
    for (int i = 0; i < 32; i++) seed[32 + i] = tls_conn.server_random[i];
    tls_prf(tls_conn.master_secret, 48, premaster, 32, "master secret", seed, 64);

    u8 key_block[2 * (16 + 4)];
    u8 seed2[64];
    for (int i = 0; i < 32; i++) seed2[i] = tls_conn.server_random[i];
    for (int i = 0; i < 32; i++) seed2[32 + i] = tls_conn.client_random[i];
    tls_prf(key_block, sizeof(key_block), tls_conn.master_secret, 48, "key expansion", seed2, 64);

    u32 pos = 0;
    aes128_set_key(&tls_conn.client_write_aes, key_block + pos); pos += 16;
    aes128_set_key(&tls_conn.server_write_aes, key_block + pos); pos += 16;
    for (int i = 0; i < 4; i++) { tls_conn.client_write_iv[i] = key_block[pos + i]; }
    pos += 4;
    for (int i = 0; i < 4; i++) { tls_conn.server_write_iv[i] = key_block[pos + i]; }
    pos += 4;
}

/* Computes a Finished message's 12-byte verify_data: PRF(master_secret,
 * label, SHA-256(transcript so far))[0:12]. `label` is "client
 * finished" for ours, "server finished" for verifying the server's --
 * same construction, different label and different point in the
 * transcript (ours is computed before our own Finished is sent/hashed;
 * the server's expected value, which we compute to check against what
 * it actually sent, is computed after OUR Finished has already been
 * folded into the transcript, matching where the server computes its
 * own copy from its point of view). */
static inline void tls_compute_finished(u8 out[12], const char *label) {
    sha256_ctx_t snapshot = tls_conn.transcript; /* copy -- sha256_final()
                                                  * mutates, and the real
                                                  * transcript must keep
                                                  * accumulating afterward */
    u8 transcript_hash[32];
    sha256_final(&snapshot, transcript_hash);

    u8 full[32];
    tls_prf(full, 32, tls_conn.master_secret, 48, label, transcript_hash, 32);
    for (int i = 0; i < 12; i++) out[i] = full[i];
}

static inline void tls_send_finished(void) {
    u8 verify_data[12];
    tls_compute_finished(verify_data, "client finished");
    /* ChangeCipherSpec is its own content type, not a handshake message
     * -- one byte (0x01), unencrypted (we're not yet writing encrypted,
     * since write_cipher_active flips to 1 right after this, not
     * before), and NOT folded into the transcript hash (only actual
     * handshake-content-type messages are). */
    u8 ccs = 0x01;
    tls_write_record(TLS_CONTENT_CHANGE_CIPHER_SPEC, &ccs, 1);
    tls_conn.write_cipher_active = 1;
    tls_conn.client_seq = 0;
    tls_send_handshake(TLS_HS_FINISHED, verify_data, 12);
}

/* Called once we've received and decrypted the server's Finished
 * message: recomputes what its verify_data SHOULD be (from our own
 * accumulated transcript, which by this point includes our ClientHello
 * through our own Finished, but not yet the server's) and compares.
 * Mismatch here means either a transcript-hash bug on one side or -- the
 * entire point of this check existing -- active tampering with the
 * handshake; either way, this connection cannot be trusted and fails. */
static inline int tls_verify_server_finished(const u8 *received_verify_data, u32 len) {
    if (len != 12) return 0;
    u8 expected[12];
    tls_compute_finished(expected, "server finished");
    u8 diff = 0;
    for (int i = 0; i < 12; i++) diff |= (u8)(expected[i] ^ received_verify_data[i]);
    return diff == 0;
}

/* Decrypts one already-extracted record's payload in place (for
 * content types handshake/application_data, once read_cipher_active).
 * `payload` is the record's full payload (explicit nonce + ciphertext +
 * tag); on success, the first 8 bytes are consumed as the nonce and the
 * ciphertext portion is overwritten with plaintext, and this returns a
 * pointer to that plaintext plus its length via out params. Returns 0
 * (and sets TLS_FAIL_DECRYPT) on a bad tag -- callers must not use
 * anything from `payload` if this returns 0. */
static inline int tls_decrypt_record(u8 content_type, u8 *payload, u32 payload_len,
                                       u8 **out_plain, u32 *out_len) {
    if (payload_len < 8 + 16) { tls_conn.fail_reason = TLS_FAIL_UNEXPECTED_MESSAGE; return 0; }
    u8 *explicit_nonce = payload;
    u8 *ciphertext = payload + 8;
    u32 ct_len = payload_len - 8 - 16;
    u8 *tag = payload + 8 + ct_len;

    u8 nonce[12];
    for (int i = 0; i < 4; i++) nonce[i] = tls_conn.server_write_iv[i];
    for (int i = 0; i < 8; i++) nonce[4 + i] = explicit_nonce[i];

    u8 aad[13];
    for (int i = 0; i < 8; i++) aad[i] = (u8)(tls_conn.server_seq >> (56 - i * 8));
    aad[8] = content_type;
    aad[9] = (u8)(TLS_VERSION_1_2 >> 8);
    aad[10] = (u8)(TLS_VERSION_1_2 & 0xFF);
    aad[11] = (u8)(ct_len >> 8);
    aad[12] = (u8)(ct_len & 0xFF);

    if (!gcm_decrypt(&tls_conn.server_write_aes, nonce, aad, 13, ciphertext, ct_len, tag)) {
        tls_conn.fail_reason = TLS_FAIL_DECRYPT;
        return 0;
    }
    tls_conn.server_seq++;
    *out_plain = ciphertext;
    *out_len = ct_len;
    return 1;
}

/* Dispatches exactly one complete handshake message (already reassembled
 * in tls_conn.hs_buf) according to the current state. Advances
 * tls_conn.state on success; sets TLS_FAILED + a reason on anything
 * unexpected. This is the heart of the handshake's actual protocol
 * logic -- everything above this function is plumbing (framing,
 * crypto primitives) this function calls in the right order.
 *
 * Takes `now_packed` (the current YYYYMMDDHHMMSS date, from RTC) purely
 * to hand to tls_verify_certificate_chain() the moment the Certificate
 * message finishes parsing -- verifying immediately, rather than
 * waiting for ServerHelloDone, means a chain that fails validation
 * stops this handshake right there instead of after also processing
 * whatever ServerKeyExchange the server sent for a certificate we're
 * about to reject anyway. */
static inline void tls_dispatch_handshake_message(u8 msg_type, const u8 *body, u32 body_len, u64 now_packed) {
    switch (tls_conn.state) {
        case TLS_CLIENT_HELLO_SENT:
            if (msg_type == TLS_HS_SERVER_HELLO) {
                if (!tls_parse_server_hello(body, body_len)) { tls_conn.state = TLS_FAILED; return; }
            } else if (msg_type == TLS_HS_CERTIFICATE) {
                if (!tls_parse_certificate_message(body, body_len)) { tls_conn.state = TLS_FAILED; return; }
                if (!tls_verify_certificate_chain(now_packed)) { tls_conn.state = TLS_FAILED; return; }
            } else if (msg_type == TLS_HS_SERVER_KEY_EXCHANGE) {
                if (!tls_parse_server_key_exchange(body, body_len)) { tls_conn.state = TLS_FAILED; return; }
            } else if (msg_type == TLS_HS_CERTIFICATE_REQUEST) {
                /* Client certificate auth: not supported, but also not
                 * fatal by itself -- a server asking doesn't mean it
                 * requires one. We simply never send a Certificate
                 * message of our own; most servers accept that as "no
                 * client cert offered" and proceed. */
            } else if (msg_type == TLS_HS_SERVER_HELLO_DONE) {
                /* Full server flight received, chain and SKE signature
                 * already verified as each message arrived above -- now
                 * do the key exchange: generate our ephemeral X25519
                 * keypair, compute the shared secret against the
                 * server's ephemeral public key, derive the session
                 * keys, and respond. */
                tls_rng_bytes(tls_conn.my_private, 32);
                x25519_derive_public(tls_conn.my_public, tls_conn.my_private);
                u8 premaster[32];
                x25519_scalarmult(premaster, tls_conn.my_private, tls_conn.peer_public);
                tls_derive_keys(premaster);

                tls_send_client_key_exchange();
                tls_send_finished();
                tls_conn.state = TLS_CLIENT_FINISHED_SENT;
            } else {
                tls_conn.fail_reason = TLS_FAIL_UNEXPECTED_MESSAGE;
                tls_conn.state = TLS_FAILED;
            }
            break;

        case TLS_CLIENT_FINISHED_SENT:
            if (msg_type == TLS_HS_FINISHED) {
                if (!tls_verify_server_finished(body, body_len)) {
                    tls_conn.fail_reason = TLS_FAIL_SERVER_FINISHED;
                    tls_conn.state = TLS_FAILED;
                    return;
                }
                tls_conn.state = TLS_ESTABLISHED;
            } else {
                tls_conn.fail_reason = TLS_FAIL_UNEXPECTED_MESSAGE;
                tls_conn.state = TLS_FAILED;
            }
            break;

        default:
            /* a handshake message in ESTABLISHED (e.g. a renegotiation
             * request) or any other state this client doesn't expect
             * one in -- ignored rather than failing the connection,
             * since post-handshake HelloRequest messages are legal
             * (if rare) and this client simply never renegotiates */
            break;
    }
}

/* Splits tls_conn.hs_buf into complete handshake messages and dispatches
 * each in order, folding received messages into the transcript hash as
 * they're consumed -- mirroring tls_send_handshake()'s hashing on the
 * send side, so both directions' messages end up hashed in true wire
 * order regardless of how tcp_poll_recv() happened to chunk the
 * underlying byte stream. */
static inline void tls_process_handshake_buffer(u64 now_packed) {
    while (tls_conn.hs_buf_len >= 4) {
        u32 body_len = ((u32)tls_conn.hs_buf[1] << 16) | ((u32)tls_conn.hs_buf[2] << 8) | tls_conn.hs_buf[3];
        u32 total = 4 + body_len;
        if (tls_conn.hs_buf_len < total) break; /* message not fully arrived yet */

        u8 msg_type = tls_conn.hs_buf[0];

        /* Special case: the server's own Finished message. Per RFC 5246
         * 7.4.9, a Finished message's verify_data covers every handshake
         * message *up to but not including* the Finished message itself
         * -- so the transcript hash used to check it must NOT yet
         * include these exact bytes. Every other message gets folded in
         * before dispatch (the normal, simpler order); this one gets
         * dispatched (verified) first and folded in after, so
         * tls_verify_server_finished()'s snapshot is the correct
         * "everything except this" hash instead of a self-referential
         * one that could never match. */
        int is_server_finished = (tls_conn.state == TLS_CLIENT_FINISHED_SENT && msg_type == TLS_HS_FINISHED);

        if (!is_server_finished) {
            tls_transcript_update(tls_conn.hs_buf, total);
        }
        tls_dispatch_handshake_message(msg_type, tls_conn.hs_buf + 4, body_len, now_packed);
        if (is_server_finished && tls_conn.state != TLS_FAILED) {
            tls_transcript_update(tls_conn.hs_buf, total);
        }

        for (u32 i = total; i < tls_conn.hs_buf_len; i++) tls_conn.hs_buf[i - total] = tls_conn.hs_buf[i];
        tls_conn.hs_buf_len -= total;

        if (tls_conn.state == TLS_FAILED) return;
    }
}

/* Splits tls_conn.rx_raw into complete TLS records and processes each:
 * change_cipher_spec flips read_cipher_active and resets server_seq;
 * alert is inspected (a fatal alert fails the connection; close_notify
 * is treated as a clean EOF signal, same spirit as TCP's own FIN);
 * handshake content gets decrypted if needed and appended to hs_buf for
 * tls_process_handshake_buffer() to split into messages; application_data
 * gets decrypted and appended to app_recv for http.h to read. */
static inline void tls_process_raw_buffer(u64 now_packed) {
    while (tls_conn.rx_raw_len >= 5) {
        u8 content_type = tls_conn.rx_raw[0];
        u32 rec_len = ((u32)tls_conn.rx_raw[3] << 8) | tls_conn.rx_raw[4];
        u32 total = 5 + rec_len;
        if (tls_conn.rx_raw_len < total) break;

        u8 *payload = tls_conn.rx_raw + 5;
        u8 *plain = payload;
        u32 plain_len = rec_len;

        if (content_type == TLS_CONTENT_CHANGE_CIPHER_SPEC) {
            tls_conn.read_cipher_active = 1;
            tls_conn.server_seq = 0;
        } else if (content_type == TLS_CONTENT_ALERT) {
            if (tls_conn.read_cipher_active) {
                if (!tls_decrypt_record(content_type, payload, rec_len, &plain, &plain_len)) {
                    tls_conn.state = TLS_FAILED;
                    return;
                }
            }
            if (plain_len >= 2 && plain[0] == 2 /* fatal */) {
                tls_conn.fail_reason = TLS_FAIL_UNEXPECTED_MESSAGE;
                tls_conn.state = TLS_FAILED;
                return;
            }
            /* a warning-level alert (most commonly close_notify) --
             * noted, not fatal; the peer closing their write side is
             * handled the same way TCP's own FIN is, elsewhere */
        } else if (content_type == TLS_CONTENT_HANDSHAKE || content_type == TLS_CONTENT_APPLICATION_DATA) {
            if (tls_conn.read_cipher_active) {
                if (!tls_decrypt_record(content_type, payload, rec_len, &plain, &plain_len)) {
                    tls_conn.state = TLS_FAILED;
                    return;
                }
            }
            if (content_type == TLS_CONTENT_HANDSHAKE) {
                if (tls_conn.hs_buf_len + plain_len > TLS_HS_BUF_SIZE) {
                    tls_conn.fail_reason = TLS_FAIL_BUFFER_OVERFLOW;
                    tls_conn.state = TLS_FAILED;
                    return;
                }
                for (u32 i = 0; i < plain_len; i++) tls_conn.hs_buf[tls_conn.hs_buf_len + i] = plain[i];
                tls_conn.hs_buf_len += plain_len;
            } else {
                /* Unlike hs_buf above, an oversized application_data
                 * record is NOT treated as fatal -- this client only
                 * ever wants to preview a response's opening bytes
                 * anyway (TLS_APP_RECV_BUF_SIZE matches https.h's own
                 * response buffer size, which matches http.h's), so
                 * once app_recv is full, the rest of this record (and
                 * any records after it, until the caller drains some
                 * room via tls_poll_recv_app_data()) is quietly
                 * dropped. A handshake message can never be partially
                 * discarded like this -- every byte of it is load-
                 * bearing for the transcript hash -- which is exactly
                 * why that branch above still fails the connection
                 * outright instead of truncating. */
                u32 room = TLS_APP_RECV_BUF_SIZE - tls_conn.app_recv_len;
                u32 take = plain_len < room ? plain_len : room;
                for (u32 i = 0; i < take; i++) tls_conn.app_recv[tls_conn.app_recv_len + i] = plain[i];
                tls_conn.app_recv_len += take;
            }
        }

        for (u32 i = total; i < tls_conn.rx_raw_len; i++) tls_conn.rx_raw[i - total] = tls_conn.rx_raw[i];
        tls_conn.rx_raw_len -= total;

        tls_process_handshake_buffer(now_packed);
        if (tls_conn.state == TLS_FAILED) return;
    }
}



/* ---- Public API -- the only functions kernel.c (via https.h) should
 * ever call directly. Everything above this point is this file's own
 * internal machinery. ---- */

/* Starts a new TLS connection: resets all per-connection state, seeds
 * this connection's PRNG (see the caveat on tls_rng_seed()'s own
 * comment -- `entropy_seed` should be whatever this kernel's best
 * available entropy is, e.g. a mix of RTC fields and the tick counter;
 * it does not need to be, and currently cannot be, cryptographically
 * strong), and kicks off the underlying TCP connection. Call tls_poll()
 * every main-loop iteration afterward to actually drive the handshake
 * forward; nothing here blocks. */
static inline void tls_connect(u32 ip, u16 port, const char *hostname, const u8 entropy_seed[16]) {
    tls_conn.state = TLS_WAIT_TCP;
    tls_conn.fail_reason = TLS_FAIL_NONE;
    tls_conn.chain_len = 0;
    tls_conn.rx_raw_len = 0;
    tls_conn.hs_buf_len = 0;
    tls_conn.app_recv_len = 0;
    tls_conn.client_seq = 0;
    tls_conn.server_seq = 0;
    tls_conn.write_cipher_active = 0;
    tls_conn.read_cipher_active = 0;

    u32 i = 0;
    for (; hostname[i] && i < sizeof(tls_conn.hostname) - 1; i++) tls_conn.hostname[i] = hostname[i];
    tls_conn.hostname[i] = 0;

    tls_rng_seed(entropy_seed);
    sha256_init(&tls_conn.transcript);

    tcp_connect(ip, port);
}

/* Advances the connection: checks on the underlying TCP handshake if
 * still waiting on it, otherwise drains whatever TCP bytes are
 * available into rx_raw and processes as many complete records/
 * messages as have arrived. Returns the current state, same
 * poll()-style contract as tcp_poll_retransmit()/dns_poll()/
 * http_poll() elsewhere in this stack. `now_packed` is only actually
 * used at one specific moment (right when the Certificate message
 * finishes parsing, for validity-date checking) but is threaded through
 * every call for simplicity -- reading RTC is cheap, and this avoids a
 * more complex "only pass it in sometimes" calling convention. */
static inline tls_state_t tls_poll(u64 now_packed) {
    if (tls_conn.state == TLS_WAIT_TCP) {
        if (tcp_conn.state == TCP_ESTABLISHED) {
            tls_send_client_hello();
            tls_conn.state = TLS_CLIENT_HELLO_SENT;
        } else if (tcp_conn.state == TCP_CLOSED) {
            tls_conn.fail_reason = TLS_FAIL_TCP;
            tls_conn.state = TLS_FAILED;
        }
        return tls_conn.state;
    }

    if (tls_conn.state == TLS_CLIENT_HELLO_SENT || tls_conn.state == TLS_CLIENT_FINISHED_SENT ||
        tls_conn.state == TLS_ESTABLISHED) {
        u32 room = TLS_RX_RAW_BUF_SIZE - tls_conn.rx_raw_len;
        if (room > 0) {
            u16 n = tcp_poll_recv(tls_conn.rx_raw + tls_conn.rx_raw_len, (u16)(room > 0xFFFF ? 0xFFFF : room));
            tls_conn.rx_raw_len += n;
        }
        tls_process_raw_buffer(now_packed);
    }
    return tls_conn.state;
}

/* Sends `len` bytes of application data (an HTTP request, in this
 * client's actual use) as one or more encrypted records. Only valid
 * once TLS_ESTABLISHED. Splits into TLS_MAX_RECORD_PLAINTEXT-sized
 * chunks if necessary, though this client's own HTTP requests are
 * always far smaller than that in practice. */
static inline int tls_send_app_data(const u8 *data, u32 len) {
    if (tls_conn.state != TLS_ESTABLISHED) return 0;
    u32 off = 0;
    while (off < len) {
        u32 chunk = len - off;
        if (chunk > TLS_MAX_RECORD_PLAINTEXT) chunk = TLS_MAX_RECORD_PLAINTEXT;
        if (!tls_write_record(TLS_CONTENT_APPLICATION_DATA, data + off, chunk)) return 0;
        off += chunk;
    }
    return 1;
}

/* Drains up to `maxlen` bytes of decrypted application data into `out`,
 * sliding any remainder down -- identical contract to tcp_poll_recv()
 * and http.h's own response buffer draining, so https.h (the eventual
 * http.h counterpart that speaks TLS) can reuse the same polling
 * pattern http.h already established. */
static inline u16 tls_poll_recv_app_data(u8 *out, u16 maxlen) {
    u16 n = tls_conn.app_recv_len < maxlen ? (u16)tls_conn.app_recv_len : maxlen;
    for (u16 i = 0; i < n; i++) out[i] = tls_conn.app_recv[i];
    for (u32 i = n; i < tls_conn.app_recv_len; i++) tls_conn.app_recv[i - n] = tls_conn.app_recv[i];
    tls_conn.app_recv_len -= n;
    return n;
}

/* Closes the connection: sends a close_notify alert (the polite way to
 * end a TLS connection, per RFC 5246 7.2.1 -- lets the peer distinguish
 * "connection ended cleanly" from "connection was cut off mid-stream,
 * possibly by an attacker truncating the response") if we ever finished
 * the handshake, then closes the underlying TCP connection the same way
 * http.h already does via tcp_close(). */
static inline void tls_close(void) {
    if (tls_conn.write_cipher_active) {
        u8 alert[2] = { 0x01, 0x00 }; /* level=warning, description=close_notify */
        tls_write_record(TLS_CONTENT_ALERT, alert, 2);
    }
    tcp_close();
    tls_conn.state = TLS_CLOSED;
}

#endif
