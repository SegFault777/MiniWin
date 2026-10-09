#ifndef TCP_H
#define TCP_H
#include "io.h"
#include "net.h"
#include "ip.h"
#include "serial.h"
#include "memmap.h"

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
 *   - a real SEND QUEUE (tcp_send_data() appends, tcp_pump_send()
 *     drains it into segments), but still stop-and-wait on the wire:
 *     one un-ACKed segment in flight at a time, one retransmit timer,
 *     a fixed number of retries before giving up -- no sliding window,
 *     no congestion control, because this kernel is not about to
 *     saturate a gigabit link fighting other flows for bandwidth
 *   - a single receive buffer the caller drains by polling, not a
 *     socket API with blocking reads -- and, since pre-20, an honest
 *     advertised window (whatever room is actually left in it) instead
 *     of a fixed "sure, send more" that silently dropped the overflow
 *
 * This is enough TCP to fetch a web page. It is not enough TCP to
 * replace an OS you'd trust with anything that matters. Both of those
 * facts are the point.
 * ============================================================ */

#define TCP_HDR_LEN_MIN 20   /* a plain header, no options */
#define TCP_SYN_HDR_LEN 24   /* header + one 4-byte MSS option -- only the SYN
                              * carries options; everything else stays plain */

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10

/* MSS, in three flavors, because "how big is a segment" turns out to be
 * three different questions:
 *
 *   TCP_RECV_MSS  what we TELL the server it may send us (SYN option).
 *                 Before pre-20 we advertised nothing, so every server
 *                 fell back to RFC 879's 536-byte default and dribbled
 *                 a 100KB page out as ~190 tiny segments. 1400 is a bit
 *                 under a 1500-byte Ethernet MTU minus 40 bytes of
 *                 headers (1460), on purpose: a VPN or PPPoE hop
 *                 somewhere between here and the server quietly eating
 *                 40-100 bytes of MTU is common, and this kernel has no
 *                 path-MTU discovery to notice.
 *   TCP_SEND_MSS  the most we ever put in ONE outgoing segment: the
 *                 smaller of what the peer said it accepts and this
 *                 cap. Our outgoing traffic is tiny (a TLS handshake
 *                 flight, an HTTP request), so a conservative 1200
 *                 costs nothing and dodges the MTU question entirely.
 *   TCP_MSS_DEFAULT what a peer that sent no MSS option gets assumed to
 *                 accept -- RFC 879's 536, the value every host on
 *                 Earth is required to handle. */
#define TCP_RECV_MSS    1400
#define TCP_SEND_MSS    1200
#define TCP_MSS_DEFAULT 536

/* Both buffers live up in the net arena (kernel/memmap.h), not in .bss --
 * .bss has ~14KB of headroom left and these want tens of KB. They are
 * plain flat byte arrays at fixed physical addresses (paging is off, so
 * an address IS a pointer). */
#define TCP_RECV_BUF_SIZE MW_TCP_RECV_SIZE   /* 32KB of not-yet-read inbound bytes */
#define TCP_SEND_BUF_SIZE MW_TCP_SEND_SIZE   /* 20KB of queued/in-flight outbound bytes --
                                              * room for one maximum-size TLS record
                                              * (16413 bytes) plus a handshake flight */
#define TCP_RECV_BUF ((u8 *)MW_TCP_RECV_ADDR)
#define TCP_SEND_BUF ((u8 *)MW_TCP_SEND_ADDR)

#define TCP_MAX_RETRIES 5
#define TCP_TX_FAIL_BACKOFF_TICKS 200   /* wait this long before offering a refused segment to the NIC again */
#define TCP_MAX_TX_FAILS          20    /* ...and give the connection up after this many refusals in a row (~ the same total wait as the retransmit budget) */
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

    u16 peer_mss;     /* from the peer's SYN-ACK option, or TCP_MSS_DEFAULT */

    /* The outbound queue: send_buf[0 .. send_len) are bytes the
     * application handed us that the peer hasn't ACKed yet. The first
     * `retx_len` of them (when retx_data is set) are the segment
     * currently on the wire -- kept in the queue, not copied off to the
     * side, so a retransmit just re-reads them from the front. */
    u32 send_len;
    int fin_pending;  /* tcp_close() was called; FIN goes out once the queue drains */

    /* The one segment currently in flight (stop-and-wait). For a SYN or
     * a FIN, retx_len is 0 and there's nothing to re-read; for a data
     * segment, retx_len bytes from the front of send_buf are it. */
    u16 retx_len;
    u8  retx_flags;
    int retx_pending;
    u32 retx_tick_sent;
    int retx_count;

    /* Send FAILURES (ip_send() == IP_SEND_FAILED: no address, NIC refused/timed out, ...) are not
     * retransmissions -- nothing reached the wire, so there is nothing to retransmit and no
     * sequence space was used. They are retried on a gentle backoff and, if the transmit path stays
     * dead, the connection is dropped instead of waiting forever on a segment that never left. */
    int tx_fail_count;
    u32 tx_next_tick;

    /* Inbound bytes: written at recv_len by tcp_handle_packet(), drained
     * from the front by tcp_poll_recv() -- which slides the rest down.
     * A ring buffer would dodge that copy; the copy is a few KB per
     * main-loop tick, which this kernel can afford, and a flat buffer
     * can't get its wrap-around logic wrong. */
    u32 recv_len;

    int peer_fin_seen;   /* the peer has sent FIN -- no more data coming */
} tcp_conn_t;

static tcp_conn_t tcp_conn;

/* Each new connection gets a fresh local port and a fresh ISN. Reusing
 * the same 4-tuple with the same sequence numbers right after a prior
 * connection closed makes a server that still remembers the old one (it
 * sits in TIME_WAIT for a while) silently ignore our SYN -- the second
 * fetch from the same site would just hang. Not random, not secret --
 * just "different from last time," which is all that problem needs. */
static u16 tcp_next_port = 49152;

/* How much room the peer may still fill -- exactly what we advertise as
 * our window. */
/* ...but never more than TCP_ADV_WINDOW_MAX. The receive BUFFER is 32KB, but the NIC's own receive
 * ring (rtl8139.h: 32KB) has to hold everything the peer is allowed to have in flight, because the
 * driver is only polled once per main-loop pass. 20KB is ~15 full frames (~22KB on the wire), which
 * fits with room to spare. Advertising the full 32KB let a burst overrun the ring and lose frames. */
#define TCP_ADV_WINDOW_MAX 20480
static inline u16 tcp_recv_window(void) {
    u32 room = TCP_RECV_BUF_SIZE - tcp_conn.recv_len;
    if (room > TCP_ADV_WINDOW_MAX) room = TCP_ADV_WINDOW_MAX;
    return (u16)room;
}

/* How many more bytes tcp_send_data() will accept right now. */
static inline u32 tcp_send_space(void) {
    return TCP_SEND_BUF_SIZE - tcp_conn.send_len;
}

/* Builds and sends one TCP segment for the current connection. `flags`
 * are the control bits (SYN/ACK/FIN/...), `data`/`data_len` is the
 * payload (0 bytes for a bare ACK or SYN). Does NOT touch snd_nxt or
 * the retransmit slot -- callers that need those updated (i.e.
 * everything except a pure retransmit) do that themselves, since a
 * plain retransmit needs to resend the exact same bytes with the exact
 * same sequence number. A SYN additionally carries the MSS option. */
/* Returns ip_send()'s verdict: IP_SEND_SENT, IP_SEND_QUEUED (parked behind ARP; it WILL go out) or
 * IP_SEND_FAILED (it did not and will not leave on its own). Callers that advance snd_nxt / arm the
 * retransmit timer do so only when this is not IP_SEND_FAILED -- recording a segment that never left
 * as "in flight" is how the wire and the TCP state used to drift apart. */
static inline int tcp_send_segment(u8 flags, const u8 *data, u16 data_len) {
    u8 seg[TCP_SYN_HDR_LEN + TCP_SEND_MSS];
    u16 hdr_len = (flags & TCP_FLAG_SYN) ? TCP_SYN_HDR_LEN : TCP_HDR_LEN_MIN;
    if (data_len > TCP_SEND_MSS) data_len = TCP_SEND_MSS; /* never happens by construction; belt and braces */

    net_put16_be(&seg[0], tcp_conn.local_port);
    net_put16_be(&seg[2], tcp_conn.remote_port);
    net_put32_be(&seg[4], tcp_conn.snd_nxt);
    net_put32_be(&seg[8], (flags & TCP_FLAG_ACK) ? tcp_conn.rcv_nxt : 0);
    seg[12] = (u8)((hdr_len / 4) << 4);      /* data offset, in 32-bit words */
    seg[13] = flags;
    net_put16_be(&seg[14], tcp_recv_window()); /* the truth, not a hopeful fixed number */
    net_put16_be(&seg[16], 0);               /* checksum: filled below */
    net_put16_be(&seg[18], 0);               /* urgent pointer: unused */
    if (flags & TCP_FLAG_SYN) {
        seg[20] = 2;                         /* option kind 2: Maximum Segment Size */
        seg[21] = 4;                         /* option length, including these two bytes */
        net_put16_be(&seg[22], TCP_RECV_MSS);
    }
    for (u16 i = 0; i < data_len; i++) seg[hdr_len + i] = data[i];

    u16 seg_len = (u16)(hdr_len + data_len);

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

/* Arms the retransmit slot for whatever tcp_send_segment() just sent.
 * `data_len` is how many bytes from the FRONT of the send queue that
 * segment carried (0 for SYN/FIN) -- the retransmit timer re-reads them
 * from there instead of keeping a private copy. */
static inline void tcp_arm_retransmit(u8 flags, u16 data_len) {
    tcp_conn.retx_flags = flags;
    tcp_conn.retx_len = data_len;
    tcp_conn.retx_pending = 1;
    tcp_conn.retx_tick_sent = net_ticks;
    tcp_conn.retx_count = 0;
}

static inline void tcp_clear_retransmit(void) {
    tcp_conn.retx_pending = 0;
    tcp_conn.retx_len = 0;
    tcp_conn.retx_count = 0;
}

/* Starts an outbound connection to remote_ip:remote_port. This is the
 * "active open" half of TCP's 3-way handshake -- we pick an ISN
 * (initial sequence number), send a bare SYN (carrying our MSS), and
 * wait for the SYN-ACK that tcp_handle_packet() will recognize and
 * complete the handshake from. Only one connection exists at a time
 * (see the file header for why); starting a new one while another is
 * live simply clobbers it. */
static inline void tcp_connect(u32 remote_ip, u16 remote_port) {
    tcp_conn.state = TCP_SYN_SENT;
    tcp_conn.remote_ip = remote_ip;
    tcp_conn.remote_port = remote_port;
    tcp_conn.local_port = tcp_next_port++;
    if (tcp_next_port > 60000) tcp_next_port = 49152;
    tcp_conn.snd_una = 0x1000u + (net_ticks << 8) + ((u32)tcp_conn.local_port << 16);
    tcp_conn.snd_nxt = tcp_conn.snd_una;
    tcp_conn.rcv_nxt = 0;
    tcp_conn.recv_len = 0;
    tcp_conn.send_len = 0;
    tcp_conn.fin_pending = 0;
    tcp_conn.peer_mss = TCP_MSS_DEFAULT;
    tcp_conn.peer_fin_seen = 0;
    tcp_clear_retransmit();
    tcp_conn.tx_fail_count = 0;
    tcp_conn.tx_next_tick = 0;

    serial_puts("[TCP] connecting to ");
    net_log_ip(remote_ip);
    serial_puts(":");
    serial_put_dec(remote_port);
    serial_putc('\n');

    if (tcp_send_segment(TCP_FLAG_SYN, 0, 0) == IP_SEND_FAILED) {
        /* The SYN never left (no address yet, NIC refused it...). Don't pretend a handshake is in
         * progress and sit in SYN_SENT until the retransmit budget runs out: fail now, the way the
         * callers already handle a connection that closed on them (http.h/https.h see CLOSED). */
        serial_puts("[TCP] SYN could not be sent, connect failed\n");
        tcp_conn.state = TCP_CLOSED;
        return;
    }
    tcp_arm_retransmit(TCP_FLAG_SYN, 0);
    tcp_conn.snd_nxt++; /* SYN consumes one sequence number, per RFC 793 --
                         * yes, even though it carries no data */
}

/* Books one refused transmission: back off before the next try, and after TCP_MAX_TX_FAILS in a row
 * conclude the transmit path is dead and drop the connection (callers see TCP_CLOSED and report it)
 * instead of leaving the UI waiting on bytes that can never leave. */
static inline void tcp_tx_refused(void) {
    tcp_conn.tx_fail_count++;
    tcp_conn.tx_next_tick = net_ticks + TCP_TX_FAIL_BACKOFF_TICKS;
    if (tcp_conn.tx_fail_count >= TCP_MAX_TX_FAILS) {
        serial_puts("[TCP] transmit path keeps failing, giving up\n");
        tcp_conn.state = TCP_CLOSED;
        tcp_clear_retransmit();
    }
}

/* The send queue's engine: if nothing is in flight, cut the next
 * segment off the front of the queue and put it on the wire; if the
 * queue is empty and tcp_close() asked for a FIN, send that instead.
 * Called whenever the picture changes -- bytes queued, an ACK landed,
 * the retransmit poll ticked. */
static inline void tcp_pump_send(void) {
    if (tcp_conn.state != TCP_ESTABLISHED) return;
    if (tcp_conn.retx_pending) return;          /* stop-and-wait: one at a time */

    if (tcp_conn.send_len == 0 && !tcp_conn.fin_pending) return;
    if (tcp_conn.tx_fail_count > 0 && net_ticks < tcp_conn.tx_next_tick) return;   /* backing off after a refusal */

    if (tcp_conn.send_len > 0) {
        u32 n = tcp_conn.send_len;
        u32 cap = tcp_conn.peer_mss < TCP_SEND_MSS ? tcp_conn.peer_mss : TCP_SEND_MSS;
        if (n > cap) n = cap;
        if (tcp_send_segment(TCP_FLAG_ACK | TCP_FLAG_PSH, TCP_SEND_BUF, (u16)n) == IP_SEND_FAILED) {
            tcp_tx_refused();      /* nothing left the host: no snd_nxt advance, no retransmit armed, bytes stay queued */
            return;
        }
        tcp_conn.tx_fail_count = 0;
        tcp_arm_retransmit(TCP_FLAG_ACK | TCP_FLAG_PSH, (u16)n);
        tcp_conn.snd_nxt += n;
    } else if (tcp_conn.fin_pending) {
        if (tcp_send_segment(TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0) == IP_SEND_FAILED) {
            tcp_tx_refused();      /* fin_pending stays set: the FIN is offered again after the backoff */
            return;
        }
        tcp_conn.tx_fail_count = 0;
        tcp_conn.fin_pending = 0;
        tcp_arm_retransmit(TCP_FLAG_FIN | TCP_FLAG_ACK, 0);
        tcp_conn.snd_nxt++; /* FIN also consumes a sequence number */
        tcp_conn.state = TCP_FIN_WAIT_1;
        serial_puts("[TCP] closing\n");
    }
}

/* Appends `len` bytes to the outbound queue and kicks the pump.
 * Returns 1 if ALL of it was queued, 0 if there wasn't room for all of
 * it (nothing is queued in that case -- no half-sent records) or the
 * connection isn't up. The old version of this function silently
 * truncated anything past one 536-byte segment and refused whenever the
 * previous segment was still un-ACKed, and TLS's handshake fires three
 * writes back to back and ignored the answer -- so the second and third
 * were simply lost. A queue makes all of that a non-problem. */
static inline int tcp_send_data(const u8 *data, u16 len) {
    if (tcp_conn.state != TCP_ESTABLISHED || tcp_conn.fin_pending) return 0;
    if ((u32)len > tcp_send_space()) return 0;
    for (u16 i = 0; i < len; i++) TCP_SEND_BUF[tcp_conn.send_len + i] = data[i];
    tcp_conn.send_len += len;
    tcp_pump_send();
    return 1;
}

/* Begins closing the connection. Any queued bytes go out first (the
 * pump sends FIN once the queue is empty), then FIN, then we wait in
 * FIN_WAIT_1/2 for the peer's half of the goodbye -- the 4-way close,
 * with tcp_handle_packet() playing the peer's parts as they arrive. */
static inline void tcp_close(void) {
    if (tcp_conn.state != TCP_ESTABLISHED) return;
    tcp_conn.fin_pending = 1;
    tcp_pump_send();
}

/* Copies up to `maxlen` bytes of whatever's arrived into `out`, and
 * slides the remaining buffered bytes down to the front. Returns how
 * many bytes were actually copied. This is the closest thing this
 * kernel has to a recv() call -- callers (http.h) poll it in a loop
 * until either they have a full response or peer_fin_seen tells them
 * no more is coming. If the window had shrunk to nothing (the buffer
 * was full), draining it reopens the window -- send a bare ACK to say
 * so, or a peer that has stopped sending at zero window will wait
 * for its persist timer instead of hearing about it right now. */
static inline u16 tcp_poll_recv(u8 *out, u16 maxlen) {
    u32 n = tcp_conn.recv_len < maxlen ? tcp_conn.recv_len : maxlen;
    for (u32 i = 0; i < n; i++) out[i] = TCP_RECV_BUF[i];
    for (u32 i = n; i < tcp_conn.recv_len; i++) TCP_RECV_BUF[i - n] = TCP_RECV_BUF[i];
    u32 was_full_ish = (TCP_RECV_BUF_SIZE - tcp_conn.recv_len) < TCP_RECV_MSS;
    tcp_conn.recv_len -= n;
    if (n > 0 && was_full_ish && tcp_conn.state == TCP_ESTABLISHED) {
        tcp_send_segment(TCP_FLAG_ACK, 0, 0);   /* window update */
    }
    return (u16)n;
}

/* Pulls the peer's MSS out of a SYN-ACK's options, if it sent one.
 * Options are a little TLV list between the fixed 20-byte header and
 * the payload: kind 0 = end, kind 1 = one-byte no-op padding, anything
 * else = {kind, length, data...}. */
static inline void tcp_parse_syn_options(const u8 *seg, u16 hdr_len) {
    u16 i = TCP_HDR_LEN_MIN;
    while (i < hdr_len) {
        u8 kind = seg[i];
        if (kind == 0) break;
        if (kind == 1) { i++; continue; }
        if (i + 1 >= hdr_len) break;
        u8 olen = seg[i + 1];
        if (olen < 2 || i + olen > hdr_len) break;
        if (kind == 2 && olen == 4) {
            u16 mss = net_get16_be(&seg[i + 2]);
            if (mss >= 64) tcp_conn.peer_mss = mss;
        }
        i = (u16)(i + olen);
    }
}

/* Verifies the TCP checksum of an incoming segment: sum the same
 * pseudo-header tcp_send_segment() builds (source IP, destination IP,
 * zero, protocol 6, segment length) plus the whole segment, checksum
 * field included -- a segment that survived the trip intact folds to
 * all-ones, i.e. net_checksum_finish() returns 0. Without this, a frame
 * mangled on the wire (or a lazily forged one) went straight into the
 * state machine as if it were gospel. Returns 1 if the checksum is good. */
static inline int tcp_checksum_ok(const ip_packet_t *ip) {
    u8 pseudo[12];
    net_put32_be(&pseudo[0], ip->src_ip);
    net_put32_be(&pseudo[4], ip->dst_ip);
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_TCP;
    net_put16_be(&pseudo[10], ip->payload_len);
    u32 sum = net_checksum_add(0, pseudo, sizeof(pseudo));
    sum = net_checksum_add(sum, ip->payload, ip->payload_len);
    return net_checksum_finish(sum) == 0;
}

/* Called by the IP dispatcher for every incoming segment addressed to
 * our one live connection's port. Walks the textbook TCP state
 * transitions for exactly the states this kernel implements (see the
 * file header). Inbound data policy, in order of how often it happens:
 *   - exactly the next expected bytes -> accept them, ACK;
 *   - bytes we already have (a retransmit, because our ACK got lost) ->
 *     don't store twice, but DO re-ACK -- staying silent here is what
 *     made the peer retransmit forever;
 *   - a gap (a segment from the future) -> we don't reassemble; re-ACK
 *     what we have (a "duplicate ACK") so the peer knows where the hole is;
 *   - no room in the receive buffer -> refuse the whole segment and
 *     re-ACK; the peer retransmits once our advertised window reopens. */
static inline void tcp_handle_packet(const ip_packet_t *ip) {
    if (ip->payload_len < TCP_HDR_LEN_MIN) return;
    const u8 *seg = ip->payload;

    u16 src_port = net_get16_be(&seg[0]);
    u16 dst_port = net_get16_be(&seg[2]);
    if (tcp_conn.state == TCP_CLOSED) return;
    if (ip->src_ip != tcp_conn.remote_ip || src_port != tcp_conn.remote_port ||
        dst_port != tcp_conn.local_port) return; /* not our connection */
    if (!tcp_checksum_ok(ip)) return;            /* corrupted or forged in transit: drop it silently, the peer will retransmit */

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

    /* Any ACK that covers everything in flight clears the retransmit
     * slot -- our one outstanding segment (SYN, data, or FIN, whichever
     * it was) just got confirmed. If it was a data segment, its bytes
     * leave the front of the send queue too. */
    if ((flags & TCP_FLAG_ACK) && ack == tcp_conn.snd_nxt && tcp_conn.retx_pending) {
        u16 acked = tcp_conn.retx_len;
        tcp_conn.snd_una = ack;
        tcp_clear_retransmit();
        if (acked > 0) {
            for (u32 i = acked; i < tcp_conn.send_len; i++) TCP_SEND_BUF[i - acked] = TCP_SEND_BUF[i];
            tcp_conn.send_len -= acked;
        }
    }

    switch (tcp_conn.state) {
        case TCP_SYN_SENT:
            /* A SYN-ACK only counts if its ACK number is exactly our ISN
             * + 1 -- i.e. it actually acknowledges the SYN we sent. The
             * 4-tuple matching isn't proof of anything on its own (ports
             * are guessable, and a stale SYN-ACK from an earlier attempt
             * to the same server carries the OLD number). Anything else
             * is dropped without a reply; our SYN is still in the
             * retransmit slot, so the real SYN-ACK gets another chance. */
            if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK) && ack == tcp_conn.snd_nxt) {
                tcp_conn.rcv_nxt = seq + 1; /* SYN consumes a sequence number, same as ours did */
                tcp_parse_syn_options(seg, hdr_len);
                tcp_conn.state = TCP_ESTABLISHED;
                serial_puts("[TCP] established, peer mss=");
                serial_put_dec(tcp_conn.peer_mss);
                serial_putc('\n');
                tcp_send_segment(TCP_FLAG_ACK, 0, 0); /* final leg of the 3-way handshake */
            }
            break;

        case TCP_ESTABLISHED:
#ifdef MW_TCP_DEBUG
            if (data_len > 0 && seq != tcp_conn.rcv_nxt) {   /* anything but the next expected bytes is worth a line */
                serial_puts("[TCPDBG] seg seq-rcv_nxt="); serial_put_dec((u32)(i32)(seq - tcp_conn.rcv_nxt));
                serial_puts(" len="); serial_put_dec(data_len);
                serial_puts(" recv_len="); serial_put_dec(tcp_conn.recv_len); serial_putc('\n');
            }
#endif
            if (data_len > 0) {
                i32 already = (i32)(tcp_conn.rcv_nxt - seq);   /* bytes of this segment we already hold */
                if (already >= 0 && (u32)already < data_len) {
                    u32 fresh = data_len - (u32)already;
                    if (fresh <= (TCP_RECV_BUF_SIZE - tcp_conn.recv_len)) {
                        const u8 *p = data + already;
                        for (u32 i = 0; i < fresh; i++) TCP_RECV_BUF[tcp_conn.recv_len + i] = p[i];
                        tcp_conn.recv_len += fresh;
                        tcp_conn.rcv_nxt += fresh;
                    }
                    /* else: no room -- fall through to the ACK below
                     * WITHOUT advancing rcv_nxt; the peer resends later */
                }
                /* every data-bearing segment gets an answer, accepted or
                 * not -- see the policy list above for why silence is wrong */
                tcp_send_segment(TCP_FLAG_ACK, 0, 0);
            }
            if ((flags & TCP_FLAG_FIN) && seq + data_len == tcp_conn.rcv_nxt) {
                /* FIN only counts once every byte before it has arrived */
                tcp_conn.rcv_nxt = seq + data_len + 1;
                tcp_conn.peer_fin_seen = 1;
                tcp_send_segment(TCP_FLAG_ACK, 0, 0);
                serial_puts("[TCP] peer closed their side\n");
                /* We stay in ESTABLISHED for our own send direction --
                 * TCP is full-duplex, the peer closing their side
                 * doesn't stop us from finishing ours. tls_close()/
                 * http.h call tcp_close() once they're done reading,
                 * which is what actually advances us out of here. */
            }
            else if ((flags & TCP_FLAG_FIN) && seq + data_len + 1 == tcp_conn.rcv_nxt) {
                /* a retransmitted FIN we already processed: our ACK got
                 * lost, so say it again or the peer keeps resending */
                tcp_send_segment(TCP_FLAG_ACK, 0, 0);
            }
            tcp_pump_send(); /* an ACK may have freed the wire for the next queued segment */
            break;

        case TCP_FIN_WAIT_1:
            if ((flags & TCP_FLAG_ACK) && ack == tcp_conn.snd_nxt) {
                /* our FIN is acknowledged; if the peer's FIN already
                 * came earlier, we're done, else wait for it */
                tcp_conn.state = tcp_conn.peer_fin_seen ? TCP_CLOSED : TCP_FIN_WAIT_2;
                if (tcp_conn.state == TCP_CLOSED) serial_puts("[TCP] closed\n");
            }
            if (data_len > 0 && seq == tcp_conn.rcv_nxt) {
                /* tail bytes that beat the peer's FIN to us; take what fits */
                u32 room = TCP_RECV_BUF_SIZE - tcp_conn.recv_len;
                if (data_len <= room) {
                    for (u16 i = 0; i < data_len; i++) TCP_RECV_BUF[tcp_conn.recv_len + i] = data[i];
                    tcp_conn.recv_len += data_len;
                    tcp_conn.rcv_nxt += data_len;
                }
                tcp_send_segment(TCP_FLAG_ACK, 0, 0);
            }
            if ((flags & TCP_FLAG_FIN) && seq + data_len == tcp_conn.rcv_nxt) {
                tcp_conn.rcv_nxt = seq + data_len + 1;
                tcp_conn.peer_fin_seen = 1;
                tcp_send_segment(TCP_FLAG_ACK, 0, 0);
                if (tcp_conn.state == TCP_FIN_WAIT_2) {
                    serial_puts("[TCP] closed (simultaneous close)\n");
                    tcp_conn.state = TCP_CLOSED;
                }
            }
            break;

        case TCP_FIN_WAIT_2:
            if (data_len > 0 && seq == tcp_conn.rcv_nxt) {
                u32 room = TCP_RECV_BUF_SIZE - tcp_conn.recv_len;
                if (data_len <= room) {
                    for (u16 i = 0; i < data_len; i++) TCP_RECV_BUF[tcp_conn.recv_len + i] = data[i];
                    tcp_conn.recv_len += data_len;
                    tcp_conn.rcv_nxt += data_len;
                }
                tcp_send_segment(TCP_FLAG_ACK, 0, 0);
            }
            if ((flags & TCP_FLAG_FIN) && seq + data_len == tcp_conn.rcv_nxt) {
                tcp_conn.rcv_nxt = seq + data_len + 1;
                tcp_conn.peer_fin_seen = 1;
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
}

/* Call once per main-loop iteration (alongside net_stack_poll()).
 * First gives the send queue a nudge (a segment that couldn't go out
 * earlier because the wire was busy may be able to now), then, if
 * there's a segment in flight that's overdue, resends it -- up to
 * TCP_MAX_RETRIES times before giving up and resetting the connection
 * to CLOSED: the same "assume the peer or the network dropped it, try
 * again" logic every real TCP stack has, just without the exponential
 * backoff a stack built for a hostile wide-area network would add. */
static inline void tcp_poll_retransmit(void) {
    tcp_pump_send();
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
     * same bytes (re-read from the front of the send queue for a data
     * segment; SYN and FIN carry none). The peer either never got it or
     * its ACK got lost; either way, "send it again" is the correct
     * response, not "send something different." */
    u32 saved_snd_nxt = tcp_conn.snd_nxt;
    tcp_conn.snd_nxt = tcp_conn.snd_una;
    tcp_send_segment(tcp_conn.retx_flags, tcp_conn.retx_len ? TCP_SEND_BUF : 0, tcp_conn.retx_len);
    tcp_conn.snd_nxt = saved_snd_nxt;

    tcp_conn.retx_tick_sent = net_ticks;
    tcp_conn.retx_count++;
    serial_puts("[TCP] retransmitting (attempt ");
    serial_put_dec((u32)tcp_conn.retx_count);
    serial_puts(")\n");
}

#endif
