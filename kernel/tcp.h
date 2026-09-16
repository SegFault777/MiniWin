#ifndef TCP_H
#define TCP_H
#include "io.h"
#include "net.h"
#include "ip.h"
#include "serial.h"

/* ============================================================
 * tcp.h -- TCP, or: the protocol that promises the things UDP refuses
 * to. Ordering, retransmission, and a connection that both ends agree
 * exists before either one sends anything real. This is the layer
 * where "no dependencies" gets its most honest test -- a real TCP/IP
 * stack (Linux's, say) is tens of thousands of lines handling
 * congestion control, selective ACKs, window scaling, and a dozen
 * other RFCs written by people who'd seen this protocol fail in ways
 * this kernel will never encounter. None of that is here.
 *
 * What IS here, on purpose, and nothing more:
 *   - exactly ONE connection at a time (single global tcp_conn_t --
 *     this kernel has no use yet for two things talking over TCP
 *     simultaneously, and "no use for it" beats "wrote it anyway")
 *   - active opens only (we connect OUT to a server; nothing in this
 *     kernel listens for incoming TCP yet)
 *   - a textbook state machine: CLOSED -> SYN_SENT -> ESTABLISHED ->
 *     FIN_WAIT -> CLOSED, the exact path an HTTP GET actually walks
 *   - one un-ACKed segment in flight at a time, one retransmit timer,
 *     a fixed number of retries before giving up -- no sliding window,
 *     no congestion control, because this kernel is not about to
 *     saturate a gigabit link fighting other flows for bandwidth
 *   - a single receive buffer the caller drains by polling, not a
 *     socket API with blocking reads
 *
 * This is enough TCP to fetch a web page. It is not enough TCP to
 * replace an OS you'd trust with anything that matters. Both of those
 * facts are the point.
 * ============================================================ */

#define TCP_HDR_LEN_MIN 20   /* no options -- MSS negotiation, window
                              * scaling, SACK, timestamps: all skipped.
                              * A hardcoded, conservative MSS (see
                              * TCP_MSS below) does the job options
                              * would otherwise negotiate. */

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10

/* Maximum Segment Size: how many payload bytes we'll ever cram into one
 * TCP segment. 536 is the RFC 879 "default MSS" every TCP stack is
 * required to accept without any negotiation at all -- by never sending
 * more than that, we never need to negotiate anything, and never need
 * to worry about IP fragmentation either (536 + 20-byte TCP header +
 * 20-byte IP header = 576, exactly the smallest MTU any IPv4 host is
 * required to support). Slower than a stack that negotiates a fatter
 * MSS over a modern Ethernet's 1500-byte MTU; simpler than one that
 * does. */
#define TCP_MSS 536

#define TCP_RECV_BUF_SIZE 4096   /* enough for an HTTP response to a
                                  * small GET -- this isn't a download
                                  * manager */

#define TCP_MAX_RETRIES 5
#define TCP_RETRANSMIT_TICKS 800   /* roughly a couple hundred main-loop
                                    * iterations' worth of patience --
                                    * see net_stack_tick() in net.h for
                                    * what a "tick" actually measures
                                    * here */

typedef enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,   /* we sent FIN, haven't seen it ACKed yet */
    TCP_FIN_WAIT_2,   /* our FIN was ACKed, waiting on the peer's FIN back */
    TCP_TIME_WAIT,    /* saw the peer's FIN, ACKed it -- textbook TCP
                       * would now sit here for 2*MSL before truly
                       * closing (in case that ACK got lost and the
                       * peer retransmits its FIN); this kernel treats
                       * TIME_WAIT as "logically closed but still
                       * willing to re-ACK a duplicate FIN," which is
                       * the part of 2MSL that actually matters for a
                       * kernel that only ever does one connection at a
                       * time and isn't about to reuse the port before
                       * the peer's fully let go of it anyway */
} tcp_state_t;

typedef struct {
    tcp_state_t state;

    u32 remote_ip;
    u16 remote_port;
    u16 local_port;

    u32 snd_una;      /* oldest byte we've sent but not yet had ACKed */
    u32 snd_nxt;      /* next sequence number we'll use to send */
    u32 rcv_nxt;      /* next sequence number we expect from the peer
                       * (i.e. what goes in our ACK field) */

    /* The one segment currently in flight, kept around so the
     * retransmit timer can resend it verbatim without the caller
     * having to remember what it sent. */
    u8  retx_buf[TCP_MSS];
    u16 retx_len;
    u8  retx_flags;
    int retx_pending;
    u32 retx_tick_sent;
    int retx_count;

    /* Inbound data, handed to us by tcp_handle_packet() and drained by
     * whatever's calling tcp_poll_recv() -- an HTTP client, in
     * practice. Simple ring-free flat buffer: written to at recv_len,
     * read from at recv_read, memmove'd down on a full drain. Small and
     * a bit wasteful under partial reads; entirely adequate for
     * receiving one HTTP response into memory. */
    u8  recv_buf[TCP_RECV_BUF_SIZE];
    u16 recv_len;

    int peer_fin_seen;   /* the peer has sent FIN -- no more data coming */
} tcp_conn_t;

static tcp_conn_t tcp_conn;

/* Builds and sends one TCP segment for the current connection. `flags`
 * are the control bits (SYN/ACK/FIN/...), `data`/`data_len` is the
 * payload (0 bytes for a bare ACK or SYN). Does NOT touch snd_nxt or
 * the retransmit slot -- callers that need those updated (i.e.
 * everything except a pure retransmit) do that themselves, since a
 * plain retransmit needs to resend the exact same bytes with the exact
 * same sequence number. */
static inline int tcp_send_segment(u8 flags, const u8 *data, u16 data_len) {
    u8 seg[TCP_HDR_LEN_MIN + TCP_MSS];
    net_put16_be(&seg[0], tcp_conn.local_port);
    net_put16_be(&seg[2], tcp_conn.remote_port);
    net_put32_be(&seg[4], tcp_conn.snd_nxt);
    net_put32_be(&seg[8], (flags & TCP_FLAG_ACK) ? tcp_conn.rcv_nxt : 0);
    seg[12] = (TCP_HDR_LEN_MIN / 4) << 4;   /* data offset: 5 words, no options */
    seg[13] = flags;
    net_put16_be(&seg[14], 8192);            /* window: fixed and generous --
                                               * we're not doing real flow
                                               * control, just promising
                                               * "yes, room for more" */
    net_put16_be(&seg[16], 0);               /* checksum: filled below */
    net_put16_be(&seg[18], 0);               /* urgent pointer: unused */
    for (u16 i = 0; i < data_len; i++) seg[TCP_HDR_LEN_MIN + i] = data[i];

    u16 seg_len = (u16)(TCP_HDR_LEN_MIN + data_len);

    /* Same pseudo-header ritual as UDP (see udp.h's udp_send for the
     * fuller explanation) -- TCP's checksum vouches for source/dest IP
     * too, not just its own header and payload. */
    u8 pseudo[12];
    net_put32_be(&pseudo[0], net_cfg.my_ip);
    net_put32_be(&pseudo[4], tcp_conn.remote_ip);
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_TCP;
    net_put16_be(&pseudo[10], seg_len);

    u32 sum = net_checksum_add(0, pseudo, sizeof(pseudo));
    sum = net_checksum_add(sum, seg, seg_len);
    net_put16_be(&seg[16], net_checksum_finish(sum));

    return ip_send(tcp_conn.remote_ip, IP_PROTO_TCP, seg, seg_len);
}

/* Arms the retransmit slot with exactly what tcp_send_segment() just
 * sent, so the timer in tcp_poll_retransmit() can resend it unchanged
 * if no ACK shows up in time. */
static inline void tcp_arm_retransmit(u8 flags, const u8 *data, u16 data_len) {
    tcp_conn.retx_flags = flags;
    tcp_conn.retx_len = data_len;
    for (u16 i = 0; i < data_len; i++) tcp_conn.retx_buf[i] = data[i];
    tcp_conn.retx_pending = 1;
    tcp_conn.retx_tick_sent = net_ticks;
    tcp_conn.retx_count = 0;
}

static inline void tcp_clear_retransmit(void) {
    tcp_conn.retx_pending = 0;
    tcp_conn.retx_count = 0;
}

/* Starts an outbound connection to remote_ip:remote_port. This is the
 * "active open" half of TCP's 3-way handshake -- we pick an ISN
 * (initial sequence number), send a bare SYN, and wait for the SYN-ACK
 * that tcp_handle_packet() will recognize and complete the handshake
 * from. Only one connection exists at a time (see the file header for
 * why); starting a new one while another is live simply clobbers it. */
static inline void tcp_connect(u32 remote_ip, u16 remote_port) {
    tcp_conn.state = TCP_SYN_SENT;
    tcp_conn.remote_ip = remote_ip;
    tcp_conn.remote_port = remote_port;
    /* A local port that won't collide with anything: not a real
     * ephemeral-port allocator (there's only ever one connection, so
     * there's nothing to collide with), just a fixed high port that
     * stays out of the well-known range. */
    tcp_conn.local_port = 49152;
    tcp_conn.snd_una = 0x1000; /* ISN -- doesn't need to be random for a
                               * kernel that isn't defending against
                               * sequence-number-guessing attacks on a
                               * connection it initiates itself */
    tcp_conn.snd_nxt = tcp_conn.snd_una;
    tcp_conn.rcv_nxt = 0;
    tcp_conn.recv_len = 0;
    tcp_conn.peer_fin_seen = 0;
    tcp_clear_retransmit();

    serial_puts("[TCP] connecting to ");
    net_log_ip(remote_ip);
    serial_puts(":");
    serial_put_dec(remote_port);
    serial_putc('\n');

    tcp_send_segment(TCP_FLAG_SYN, 0, 0);
    tcp_arm_retransmit(TCP_FLAG_SYN, 0, 0);
    tcp_conn.snd_nxt++; /* SYN consumes one sequence number, per RFC 793 --
                         * yes, even though it carries no data */
}

/* Queues (and immediately sends) up to TCP_MSS bytes of application
 * data on the current connection. This kernel's TCP doesn't buffer
 * beyond one in-flight segment, so a caller with more than TCP_MSS
 * bytes to send (an HTTP GET request line easily fits; a request body
 * might not) needs to call this again after the first send's ACK lands
 * -- fine for this kernel's one real caller (http.h), which only ever
 * sends one short GET request per connection. */
static inline int tcp_send_data(const u8 *data, u16 len) {
    if (tcp_conn.state != TCP_ESTABLISHED) return 0;
    if (len > TCP_MSS) len = TCP_MSS; /* truncate rather than pretend we queued more */
    if (tcp_conn.retx_pending) return 0; /* still waiting on the last segment's ACK */

    tcp_send_segment(TCP_FLAG_ACK | TCP_FLAG_PSH, data, len);
    tcp_arm_retransmit(TCP_FLAG_ACK | TCP_FLAG_PSH, data, len);
    tcp_conn.snd_nxt += len;
    return 1;
}

/* Begins closing the connection: sends FIN, moves to FIN_WAIT_1. Half
 * of TCP's 4-way close (the half we initiate) -- the rest happens in
 * tcp_handle_packet() as the peer's own FIN and ACKs arrive. */
static inline void tcp_close(void) {
    if (tcp_conn.state != TCP_ESTABLISHED) return;
    tcp_send_segment(TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
    tcp_arm_retransmit(TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
    tcp_conn.snd_nxt++; /* FIN also consumes a sequence number */
    tcp_conn.state = TCP_FIN_WAIT_1;
    serial_puts("[TCP] closing\n");
}

/* Copies up to `maxlen` bytes of whatever's arrived into `out`, and
 * slides the remaining buffered bytes down to the front. Returns how
 * many bytes were actually copied. This is the closest thing this
 * kernel has to a recv() call -- callers (http.h) poll it in a loop
 * until either they have a full response or peer_fin_seen tells them
 * no more is coming. */
static inline u16 tcp_poll_recv(u8 *out, u16 maxlen) {
    u16 n = tcp_conn.recv_len < maxlen ? tcp_conn.recv_len : maxlen;
    for (u16 i = 0; i < n; i++) out[i] = tcp_conn.recv_buf[i];
    /* slide the rest down -- a real ring buffer would avoid this copy,
     * but HTTP responses here are small enough that it's not worth the
     * bookkeeping complexity of one */
    for (u16 i = n; i < tcp_conn.recv_len; i++) tcp_conn.recv_buf[i - n] = tcp_conn.recv_buf[i];
    tcp_conn.recv_len = (u16)(tcp_conn.recv_len - n);
    return n;
}

/* Called by the IP dispatcher for every incoming segment addressed to
 * our one live connection's port. Walks the textbook TCP state
 * transitions for exactly the states this kernel implements (see the
 * file header) -- anything outside that (simultaneous opens, out-of-
 * order segments needing reassembly, a peer retransmitting a segment
 * we've already ACKed) either doesn't arise for a single client
 * connection talking to a well-behaved server, or is handled by the
 * blunt-but-correct instrument of "if the sequence number doesn't
 * match what we expected, drop it and let the peer's own retransmit
 * timer sort it out." */
static inline void tcp_handle_packet(const ip_packet_t *ip) {
    if (ip->payload_len < TCP_HDR_LEN_MIN) return;
    const u8 *seg = ip->payload;

    u16 src_port = net_get16_be(&seg[0]);
    u16 dst_port = net_get16_be(&seg[2]);
    if (tcp_conn.state == TCP_CLOSED) return;
    if (ip->src_ip != tcp_conn.remote_ip || src_port != tcp_conn.remote_port ||
        dst_port != tcp_conn.local_port) return; /* not our connection */

    u32 seq = net_get32_be(&seg[4]);
    u32 ack = net_get32_be(&seg[8]);
    u8 data_offset_words = (u8)(seg[12] >> 4);
    u16 hdr_len = (u16)(data_offset_words * 4);
    u8 flags = seg[13];
    if (hdr_len < TCP_HDR_LEN_MIN || hdr_len > ip->payload_len) return;
    const u8 *data = seg + hdr_len;
    u16 data_len = (u16)(ip->payload_len - hdr_len);

    if (flags & TCP_FLAG_RST) {
        serial_puts("[TCP] connection reset by peer\n");
        tcp_conn.state = TCP_CLOSED;
        tcp_clear_retransmit();
        return;
    }

    /* Any ACK that actually advances snd_una clears whatever we were
     * waiting to have acknowledged -- our one in-flight segment (SYN,
     * data, or FIN, whichever it was) just got confirmed. */
    if ((flags & TCP_FLAG_ACK) && ack == tcp_conn.snd_nxt && tcp_conn.retx_pending) {
        tcp_conn.snd_una = ack;
        tcp_clear_retransmit();
    }

    switch (tcp_conn.state) {
        case TCP_SYN_SENT:
            if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK)) {
                tcp_conn.rcv_nxt = seq + 1; /* SYN consumes a sequence number, same as ours did */
                tcp_conn.state = TCP_ESTABLISHED;
                serial_puts("[TCP] established\n");
                tcp_send_segment(TCP_FLAG_ACK, 0, 0); /* final leg of the 3-way handshake */
            }
            break;

        case TCP_ESTABLISHED:
            if (data_len > 0 && seq == tcp_conn.rcv_nxt) {
                u16 room = (u16)(TCP_RECV_BUF_SIZE - tcp_conn.recv_len);
                u16 n = data_len < room ? data_len : room; /* drop what doesn't fit --
                                                            * a real stack would shrink
                                                            * its advertised window
                                                            * instead; this one just
                                                            * trusts small HTTP
                                                            * responses to fit */
                for (u16 i = 0; i < n; i++) tcp_conn.recv_buf[tcp_conn.recv_len + i] = data[i];
                tcp_conn.recv_len = (u16)(tcp_conn.recv_len + n);
                tcp_conn.rcv_nxt += data_len;
                tcp_send_segment(TCP_FLAG_ACK, 0, 0); /* ACK every segment immediately --
                                                       * no delayed-ACK optimization,
                                                       * simplicity over throughput */
            }
            if (flags & TCP_FLAG_FIN) {
                tcp_conn.rcv_nxt = seq + data_len + 1;
                tcp_conn.peer_fin_seen = 1;
                tcp_send_segment(TCP_FLAG_ACK, 0, 0);
                serial_puts("[TCP] peer closed their side\n");
                /* We stay in ESTABLISHED for our own send direction --
                 * TCP is full-duplex, the peer closing their side
                 * doesn't stop us from finishing ours. http.h calls
                 * tcp_close() once it's done reading, which is what
                 * actually advances us out of here. */
            }
            break;

        case TCP_FIN_WAIT_1:
            if ((flags & TCP_FLAG_ACK) && ack == tcp_conn.snd_nxt) {
                tcp_conn.state = TCP_FIN_WAIT_2;
            }
            if (flags & TCP_FLAG_FIN) {
                tcp_conn.rcv_nxt = seq + 1;
                tcp_send_segment(TCP_FLAG_ACK, 0, 0);
                tcp_conn.state = (tcp_conn.state == TCP_FIN_WAIT_2) ? TCP_TIME_WAIT : TCP_FIN_WAIT_1;
                if (tcp_conn.state == TCP_TIME_WAIT) {
                    serial_puts("[TCP] closed (simultaneous close)\n");
                    tcp_conn.state = TCP_CLOSED;
                }
            }
            break;

        case TCP_FIN_WAIT_2:
            if (flags & TCP_FLAG_FIN) {
                tcp_conn.rcv_nxt = seq + 1;
                tcp_send_segment(TCP_FLAG_ACK, 0, 0);
                serial_puts("[TCP] closed\n");
                /* See TCP_TIME_WAIT's definition above for why going
                 * straight to CLOSED instead of actually waiting out
                 * 2*MSL is the right call for a kernel that only ever
                 * runs one connection at a time. */
                tcp_conn.state = TCP_CLOSED;
            }
            break;

        default:
            break;
    }
    (void)data; /* silence -Wunused in states that don't read it */
}

/* Call once per main-loop iteration (alongside net_stack_poll()).
 * Resends the one in-flight segment if too many ticks have passed
 * without an ACK, up to TCP_MAX_RETRIES times before giving up and
 * resetting the connection to CLOSED -- the same "assume the peer or
 * the network dropped it, try again" logic every real TCP stack has,
 * just without the exponential backoff a stack built for a hostile
 * wide-area network would add. */
static inline void tcp_poll_retransmit(void) {
    if (!tcp_conn.retx_pending) return;
    if (net_ticks - tcp_conn.retx_tick_sent < TCP_RETRANSMIT_TICKS) return;

    if (tcp_conn.retx_count >= TCP_MAX_RETRIES) {
        serial_puts("[TCP] giving up after too many retransmits\n");
        tcp_conn.state = TCP_CLOSED;
        tcp_clear_retransmit();
        return;
    }

    /* Resend exactly what was sent before -- same sequence number
     * (snd_una, since snd_nxt has already moved past it), same flags,
     * same bytes. The peer either never got it or its ACK got lost;
     * either way, "send it again" is the correct response, not "send
     * something different." */
    u32 saved_snd_nxt = tcp_conn.snd_nxt;
    tcp_conn.snd_nxt = tcp_conn.snd_una;
    tcp_send_segment(tcp_conn.retx_flags, tcp_conn.retx_buf, tcp_conn.retx_len);
    tcp_conn.snd_nxt = saved_snd_nxt;

    tcp_conn.retx_tick_sent = net_ticks;
    tcp_conn.retx_count++;
    serial_puts("[TCP] retransmitting (attempt ");
    serial_put_dec((u32)tcp_conn.retx_count);
    serial_puts(")\n");
}

#endif
