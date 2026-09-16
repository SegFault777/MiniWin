#ifndef HTTP_H
#define HTTP_H
#include "io.h"
#include "net.h"
#include "tcp.h"
#include "serial.h"

/* ============================================================
 * http.h -- the payoff for every layer underneath this one: an actual
 * HTTP GET, spoken over this kernel's own TCP, to a real server out on
 * whatever network DHCP found us on. This is "self-sufficient internet
 * connectivity" made literal -- boot, get an address, resolve nothing
 * (see the IP-only limitation below), connect, ask for a page, print
 * what comes back.
 *
 * Deliberately NOT here: HTTPS (this kernel has no TLS, and writing one
 * badly would be worse than not having one), chunked transfer-encoding
 * support, redirects, cookies, keep-alive, or DNS (the target is given
 * as a raw IP, not a hostname -- there's no resolver yet to turn
 * "example.com" into an address). What's here is exactly enough to GET
 * one small page from one server and see the bytes come back, which is
 * the whole point of the exercise: proving the stack underneath
 * actually works end to end, not building a browser.
 *
 * Driven as a poll()-style state machine, same philosophy as TCP itself
 * -- call http_get() once to kick a request off, then call
 * http_poll() once per main-loop iteration until it reports done.
 * Nothing here blocks; a kernel with exactly one thread of execution
 * and a GUI to keep responsive can't afford anything that does.
 * ============================================================ */

#define HTTP_RESPONSE_BUF_SIZE 4096   /* matches TCP_RECV_BUF_SIZE --
                                       * no point buffering more HTTP
                                       * response than TCP itself will
                                       * ever hand us at once anyway */

typedef enum {
    HTTP_IDLE = 0,
    HTTP_CONNECTING,
    HTTP_SENDING_REQUEST,
    HTTP_AWAITING_RESPONSE,
    HTTP_DONE,
    HTTP_FAILED,
} http_state_t;

typedef struct {
    http_state_t state;
    char host_header[64];   /* just for the Host: header -- we still
                             * connect by raw IP, this is purely what
                             * we tell the server we think we're asking for */
    char path[128];
    u8   response[HTTP_RESPONSE_BUF_SIZE];
    u16  response_len;
    int  request_sent;
} http_client_t;

static http_client_t http_client;

/* Kicks off "GET path HTTP/1.1" against server_ip:80. Sends
 * Connection: close explicitly (1.1 defaults to keep-alive, which this
 * kernel's one-shot single-buffer TCP has no use for) so the server
 * closes its end the moment the response is done -- that FIN is what
 * tells http_poll() the response is complete, the same signal HTTP/1.0
 * gives for free by always closing. 1.1 rather than 1.0 because some
 * modern servers (this was found the hard way, against a real one)
 * reject 1.0 requests outright with "426 Upgrade Required" -- HTTP/1.0
 * is old enough now that assuming a server still speaks it is no
 * longer a safe bet, even for a client this minimal. `host` populates
 * the Host: header (mandatory in 1.1, and needed for name-based virtual
 * hosting regardless); connection itself is still by raw IP, since this
 * kernel has no DNS resolver yet. */
static inline void http_get(u32 server_ip, const char *host, const char *path) {
    http_client.state = HTTP_CONNECTING;
    http_client.response_len = 0;
    http_client.request_sent = 0;

    u32 i = 0;
    for (; host[i] && i < sizeof(http_client.host_header) - 1; i++) http_client.host_header[i] = host[i];
    http_client.host_header[i] = 0;
    for (i = 0; path[i] && i < sizeof(http_client.path) - 1; i++) http_client.path[i] = path[i];
    http_client.path[i] = 0;

    serial_puts("[HTTP] GET ");
    serial_puts(path);
    serial_puts(" from ");
    net_log_ip(server_ip);
    serial_putc('\n');

    tcp_connect(server_ip, 80);
}

static inline u32 http_strlen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static inline void http_strcat(char *dst, u32 *pos, const char *src) {
    for (u32 i = 0; src[i]; i++) dst[(*pos)++] = src[i];
}

/* Call once per main-loop iteration (alongside net_stack_poll() and
 * tcp_poll_retransmit()) while a request is outstanding. Advances the
 * request through TCP's own connect/send/receive/close cycle; returns
 * the current state so the caller knows when there's a finished
 * response (or a failure) to look at. */
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
                /* Built by hand instead of with snprintf (freestanding,
                 * no libc, no snprintf) -- three http_strcat calls is
                 * plenty readable for a request this fixed-shape. */
                char req[256];
                u32 pos = 0;
                http_strcat(req, &pos, "GET ");
                http_strcat(req, &pos, http_client.path);
                http_strcat(req, &pos, " HTTP/1.1\r\nHost: ");
                http_strcat(req, &pos, http_client.host_header);
                http_strcat(req, &pos, "\r\nConnection: close\r\n\r\n");

                if (tcp_send_data((const u8*)req, (u16)pos)) {
                    http_client.request_sent = 1;
                }
                /* if tcp_send_data() declined (still waiting on
                 * tcp_conn's retx slot from the handshake's final ACK),
                 * we just try again next poll -- no harm, no separate
                 * retry counter needed here since TCP's own retransmit
                 * logic already covers "did this actually arrive" */
            } else if (tcp_conn.retx_pending == 0) {
                /* our GET was ACKed -- move on to waiting for the reply */
                http_client.state = HTTP_AWAITING_RESPONSE;
            }
            break;

        case HTTP_AWAITING_RESPONSE: {
            u16 room = (u16)(HTTP_RESPONSE_BUF_SIZE - http_client.response_len);
            if (room > 0) {
                u16 n = tcp_poll_recv(http_client.response + http_client.response_len, room);
                http_client.response_len = (u16)(http_client.response_len + n);
            }
            if (tcp_conn.peer_fin_seen && tcp_conn.recv_len == 0) {
                /* server said everything it's going to say, and we've
                 * drained every byte of it -- politely close our end
                 * and call the response complete */
                serial_puts("[HTTP] response complete, ");
                serial_put_dec(http_client.response_len);
                serial_puts(" bytes\n");
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
