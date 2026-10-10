#ifndef HTTP_H
#define HTTP_H
#include "io.h"
#include "net.h"
#include "tcp.h"
#include "serial.h"
#include "httpresp.h"

/* ============================================================
 * http.h -- the payoff for every layer underneath this one: an actual
 * HTTP GET, spoken over this kernel's own TCP, to a real server out on
 * whatever network DHCP found us on. This is "self-sufficient internet
 * connectivity" made literal -- boot, get an address, resolve a
 * hostname (see kernel/dns.h), connect, ask for a page, print what
 * comes back.
 *
 * http_get() itself still takes a raw IP, not a hostname -- turning a
 * name into an address is kernel/dns.h's job, kept as a separate layer
 * on purpose (the same reason ip.h doesn't know about ARP internally:
 * each layer owns exactly one translation, and the caller -- see
 * web_go() in kernel.c -- is the one place that actually needs to know
 * both exist and chain them together).
 *
 * Deliberately NOT here: HTTPS (this kernel has no TLS yet, and writing
 * one badly would be worse than not having one), chunked
 * transfer-encoding support, redirects, cookies, or keep-alive. What's
 * here is exactly enough to GET one small page from one server and see
 * the bytes come back, which is the whole point of the exercise:
 * proving the stack underneath actually works end to end, not building
 * a browser engine.
 *
 * Driven as a poll()-style state machine, same philosophy as TCP itself
 * -- call http_get() once to kick a request off, then call
 * http_poll() once per main-loop iteration until it reports done.
 * Nothing here blocks; a kernel with exactly one thread of execution
 * and a GUI to keep responsive can't afford anything that does.
 * ============================================================ */

#define HTTP_PATH_MAX 320   /* request path incl. query string: a search URL runs well past the old 128 */

typedef enum {
    HTTP_IDLE = 0,
    HTTP_CONNECTING,
    HTTP_SENDING_REQUEST,
    HTTP_AWAITING_RESPONSE,
    HTTP_DONE,
    HTTP_FAILED,
} http_state_t;

/* The response itself no longer lives in this struct: see httpresp.h (`hr`, HR_BUF) -- a 256KB
 * buffer shared with https.h, plus the header/chunked/redirect parsing that both transports need. */
typedef struct {
    http_state_t state;
    char host_header[80];        /* the Host: header value ("example.com" or "example.com:8080") */
    char path[HTTP_PATH_MAX];
    int  request_sent;
} http_client_t;

static http_client_t http_client;

static inline void http_get(u32 server_ip, u16 port, const char *host, const char *path) {
    http_client.state = HTTP_CONNECTING;
    hr_reset();
    http_client.request_sent = 0;

    u32 i = 0;
    for (; host[i] && i < sizeof(http_client.host_header) - 8; i++) http_client.host_header[i] = host[i];
    if (port != 80) {                           /* a non-default port belongs in the Host header, per RFC 7230 */
        http_client.host_header[i++] = ':';
        char digits[6]; int nd = 0; u32 pv = port;
        do { digits[nd++] = (char)('0' + pv % 10); pv /= 10; } while (pv && nd < 5);
        while (nd) http_client.host_header[i++] = digits[--nd];
    }
    http_client.host_header[i] = 0;
    for (i = 0; path[i] && i < sizeof(http_client.path) - 1; i++) http_client.path[i] = path[i];
    http_client.path[i] = 0;

    serial_puts("[HTTP] GET ");
    serial_puts(path);
    serial_puts(" from ");
    net_log_ip(server_ip);
    serial_putc('\n');

    tcp_connect(server_ip, port);
}

static inline http_state_t http_poll(void) {
    switch (http_client.state) {
        case HTTP_CONNECTING:
            if (tcp_conn.state == TCP_ESTABLISHED) {
                http_client.state = HTTP_SENDING_REQUEST;
            } else if (tcp_conn.state == TCP_CLOSED) {
                serial_puts("[HTTP] connect failed\n");
                http_client.state = HTTP_FAILED;
            }
            break;

        case HTTP_SENDING_REQUEST:
            if (!http_client.request_sent) {
                char req[2048];
                u32 n = hr_build_request(req, http_client.host_header, http_client.path);
                if (tcp_send_data((const u8 *)req, (u16)n)) http_client.request_sent = 1;
            } else {
                /* Don't wait for the request's ACK: a fast server's response can carry it, and the
                 * old "wait for retx_pending == 0" could stall on a lost ACK. Just start reading. */
                http_client.state = HTTP_AWAITING_RESPONSE;
            }
            break;

        case HTTP_AWAITING_RESPONSE: {
            /* Drain the transport into the shared response buffer -- up to a bounded amount per
             * call so a big page can't stall the UI loop; the rest is picked up next iteration. */
            u8 tmp[4096];
            for (int rounds = 0; rounds < 8 && !hr.complete; rounds++) {
                u16 n = tcp_poll_recv(tmp, sizeof(tmp));
                if (n == 0) break;
                hr_feed(tmp, n);
            }
            if (hr_size_fatal()) {                 /* bigger than the response buffer: fail, don't show a cut-off page */
                serial_puts("[HTTP] response too large for the buffer, failing\n");
                tcp_close();
                http_client.state = HTTP_FAILED;
                break;
            }
            int peer_done = (tcp_conn.peer_fin_seen || tcp_conn.state == TCP_CLOSED) && tcp_conn.recv_len == 0;
            if (hr.complete || peer_done) {
                if (hr.len == 0 && tcp_conn.state == TCP_CLOSED && !tcp_conn.peer_fin_seen) {
                    serial_puts("[HTTP] connection lost before any response\n");
                    http_client.state = HTTP_FAILED;
                    break;
                }
                hr_finish();
                serial_puts("[HTTP] response complete: status=");
                serial_put_dec((u32)hr.status);
                serial_puts(" bytes=");
                serial_put_dec(hr.len);
                serial_putc('\n');
                tcp_close();
                http_client.state = HTTP_DONE;
            }
            break;
        }

        default:
            break;
    }
    return http_client.state;
}

#endif
