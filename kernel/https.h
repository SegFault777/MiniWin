#ifndef HTTPS_H
#define HTTPS_H
#include "io.h"
#include "net.h"
#include "tls.h"
#include "serial.h"

/* ============================================================
 * https.h -- http.h's counterpart, speaking exactly the same HTTP/1.1
 * GET dialect but through kernel/tls.h instead of talking to
 * kernel/tcp.h directly. Structurally this is http.h with every
 * tcp_* call swapped for its tls_* equivalent (tls_connect() instead of
 * tcp_connect(), tls_send_app_data() instead of tcp_send_data(), and so
 * on) -- the HTTP-level logic (build a GET request, wait for the
 * response, notice when it's done) doesn't change at all between plain
 * and encrypted transport, which is exactly the point of having built
 * tls.h with an interface that mirrors tcp.h's own.
 *
 * Same scope limits as http.h: no redirects, no chunked
 * transfer-encoding, no keep-alive, no cookies. Plus tls.h's own limits
 * on top: one cipher suite, no session resumption, no TLS 1.3. A page
 * this can't fetch is a reason to note the limitation, not a reason to
 * silently pretend it isn't there.
 * ============================================================ */

#define HTTPS_RESPONSE_BUF_SIZE 4096   /* matches http.h's own scale --
                                        * https.h's caller (web_go() in
                                        * kernel.c) only ever wants to
                                        * show the first chunk of a
                                        * response anyway */

typedef enum {
    HTTPS_IDLE = 0,
    HTTPS_CONNECTING,       /* TLS handshake in progress */
    HTTPS_SENDING_REQUEST,
    HTTPS_AWAITING_RESPONSE,
    HTTPS_DONE,
    HTTPS_FAILED,
} https_state_t;

typedef struct {
    https_state_t state;
    char host_header[64];
    char path[128];
    u8   response[HTTPS_RESPONSE_BUF_SIZE];
    u16  response_len;
    int  request_sent;
} https_client_t;

static https_client_t https_client;

/* Kicks off "GET path HTTP/1.1" against server_ip:443 over TLS. Same
 * division of labor as http_get(): connection is by raw IP (resolve a
 * hostname with kernel/dns.h's dns_resolve() first if needed), `host`
 * populates both TLS's SNI extension (so the server picks the right
 * certificate) and the HTTP Host: header (so it picks the right
 * virtual host) -- one hostname, two protocols that both need it for
 * unrelated reasons. `entropy_seed` is forwarded straight to
 * tls_connect() -- see that function's own comment for why it doesn't
 * need to be, and currently can't be, cryptographically strong. */
static inline void https_get(u32 server_ip, const char *host, const char *path, const u8 entropy_seed[16]) {
    https_client.state = HTTPS_CONNECTING;
    https_client.response_len = 0;
    https_client.request_sent = 0;

    u32 i = 0;
    for (; host[i] && i < sizeof(https_client.host_header) - 1; i++) https_client.host_header[i] = host[i];
    https_client.host_header[i] = 0;
    for (i = 0; path[i] && i < sizeof(https_client.path) - 1; i++) https_client.path[i] = path[i];
    https_client.path[i] = 0;

    serial_puts("[HTTPS] GET ");
    serial_puts(path);
    serial_puts(" from ");
    net_log_ip(server_ip);
    serial_putc('\n');

    tls_connect(server_ip, 443, host, entropy_seed);
}

static inline u32 https_strlen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static inline void https_strcat(char *dst, u32 *pos, const char *src) {
    for (u32 i = 0; src[i]; i++) dst[(*pos)++] = src[i];
}

/* Call once per main-loop iteration while a request is outstanding,
 * same poll()-style contract as http_poll(). `now_packed` is forwarded
 * to tls_poll() for certificate validity-date checking -- see
 * tls_pack_datetime() and kernel.c's integration code for where this
 * comes from (the CMOS RTC). */
static inline https_state_t https_poll(u64 now_packed) {
    switch (https_client.state) {
        case HTTPS_CONNECTING: {
            tls_state_t ts = tls_poll(now_packed);
            if (ts == TLS_ESTABLISHED) {
                https_client.state = HTTPS_SENDING_REQUEST;
            } else if (ts == TLS_FAILED) {
                serial_puts("[HTTPS] TLS handshake failed, reason=");
                serial_put_dec((u32)tls_conn.fail_reason);
                serial_putc('\n');
                https_client.state = HTTPS_FAILED;
            }
            break;
        }

        case HTTPS_SENDING_REQUEST:
            tls_poll(now_packed);
            if (!https_client.request_sent) {
                char req[256];
                u32 pos = 0;
                https_strcat(req, &pos, "GET ");
                https_strcat(req, &pos, https_client.path);
                https_strcat(req, &pos, " HTTP/1.1\r\nHost: ");
                https_strcat(req, &pos, https_client.host_header);
                https_strcat(req, &pos, "\r\nConnection: close\r\n\r\n");

                if (tls_send_app_data((const u8*)req, pos)) {
                    https_client.request_sent = 1;
                    https_client.state = HTTPS_AWAITING_RESPONSE;
                }
                /* TLS records are sent whole (tls_write_record() doesn't
                 * have a CBC/TCP-style "still waiting on a retransmit
                 * slot" concept the way tcp_send_data() does -- the
                 * underlying TCP layer handles its own retransmission
                 * transparently underneath tls_send_app_data()), so
                 * unlike http.h there's no separate "was it ACKed yet"
                 * wait state to poll through here; a successful send
                 * moves straight to awaiting the response. */
            }
            break;

        case HTTPS_AWAITING_RESPONSE: {
            tls_state_t ts = tls_poll(now_packed);
            if (ts == TLS_FAILED) {
                serial_puts("[HTTPS] connection failed mid-response, reason=");
                serial_put_dec((u32)tls_conn.fail_reason);
                serial_putc('\n');
                https_client.state = HTTPS_FAILED;
                break;
            }
            u16 room = (u16)(HTTPS_RESPONSE_BUF_SIZE - https_client.response_len);
            if (room > 0) {
                u16 n = tls_poll_recv_app_data(https_client.response + https_client.response_len, room);
                https_client.response_len = (u16)(https_client.response_len + n);
            }
            /* Done when the underlying TCP connection has seen the
             * peer's FIN and every buffer between here and the raw
             * socket is empty -- TCP's own recv queue, TLS's raw
             * (not-yet-a-complete-record) buffer, and TLS's decrypted
             * application-data buffer. All three empty is what proves
             * there's truly nothing left to read, not just nothing
             * left *right now*. Mirrors http.h's own completion check
             * one layer up. */
            if (tcp_conn.peer_fin_seen && tcp_conn.recv_len == 0 &&
                tls_conn.rx_raw_len == 0 && tls_conn.app_recv_len == 0) {
                serial_puts("[HTTPS] response complete, ");
                serial_put_dec(https_client.response_len);
                serial_puts(" bytes\n");
                tls_close();
                https_client.state = HTTPS_DONE;
            }
            break;
        }

        default:
            break;
    }
    return https_client.state;
}

#endif
