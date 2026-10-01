#ifndef HTTPS_H
#define HTTPS_H
#include "io.h"
#include "net.h"
#include "tls.h"
#include "serial.h"
#include "httpresp.h"

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

#define HTTPS_PATH_MAX 320

typedef enum {
    HTTPS_IDLE = 0,
    HTTPS_CONNECTING,       /* TLS handshake in progress */
    HTTPS_SENDING_REQUEST,
    HTTPS_AWAITING_RESPONSE,
    HTTPS_DONE,
    HTTPS_FAILED,
} https_state_t;

/* Like http.h, the response lives in httpresp.h's shared buffer, not in here. */
typedef struct {
    https_state_t state;
    char host_header[80];
    char path[HTTPS_PATH_MAX];
    int  request_sent;
} https_client_t;

static https_client_t https_client;

static inline void https_get(u32 server_ip, u16 port, const char *host, const char *path, const u8 entropy_seed[16]) {
    https_client.state = HTTPS_CONNECTING;
    hr_reset();
    https_client.request_sent = 0;

    u32 i = 0;
    for (; host[i] && i < sizeof(https_client.host_header) - 8; i++) https_client.host_header[i] = host[i];
    if (port != 443) {
        https_client.host_header[i++] = ':';
        char digits[6]; int nd = 0; u32 pv = port;
        do { digits[nd++] = (char)('0' + pv % 10); pv /= 10; } while (pv && nd < 5);
        while (nd) https_client.host_header[i++] = digits[--nd];
    }
    https_client.host_header[i] = 0;
    for (i = 0; path[i] && i < sizeof(https_client.path) - 1; i++) https_client.path[i] = path[i];
    https_client.path[i] = 0;

    serial_puts("[HTTPS] GET ");
    serial_puts(path);
    serial_puts(" from ");
    net_log_ip(server_ip);
    serial_putc('\n');

    tls_connect(server_ip, port, host, entropy_seed);   /* SNI / name check use the bare host, not host:port */
}

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
                char req[1024];
                u32 n = hr_build_request(req, https_client.host_header, https_client.path);
                if (tls_send_app_data((const u8 *)req, n)) {
                    https_client.request_sent = 1;
                    https_client.state = HTTPS_AWAITING_RESPONSE;
                }
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
            u8 tmp[4096];
            for (int rounds = 0; rounds < 8 && !hr.complete; rounds++) {
                u16 n = tls_poll_recv_app_data(tmp, sizeof(tmp));
                if (n == 0) break;
                hr_feed(tmp, n);
            }
            int peer_done = (tcp_conn.peer_fin_seen || tls_conn.peer_close_notify || tcp_conn.state == TCP_CLOSED) &&
                            tcp_conn.recv_len == 0 && tls_conn.rx_raw_len == 0 && tls_conn.app_recv_len == 0;
            if (hr.complete || peer_done) {
                hr_finish();
                serial_puts("[HTTPS] response complete: status=");
                serial_put_dec((u32)hr.status);
                serial_puts(" bytes=");
                serial_put_dec(hr.len);
                serial_putc('\n');
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
