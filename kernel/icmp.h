#ifndef ICMP_H
#define ICMP_H
#include "io.h"
#include "net.h"
#include "ip.h"
#include "serial.h"

/* ============================================================
 * icmp.h -- ICMP, or: the entire discipline of network troubleshooting
 * reduced to "are you there? ...yes." Type 8 (echo request) goes out,
 * type 0 (echo reply) comes back, and somehow this two-message protocol
 * from 1981 is still how literally everyone's first "is the network
 * stack alive" test works, including this one's.
 *
 * We implement exactly two message types: sending echo requests (so
 * this OS can ping something) and replying to echo requests (so
 * something else can ping this OS). Destination-unreachable, time-
 * exceeded, redirects, and the rest of ICMP's extensive catalog of bad
 * news are left for a kernel with more to say.
 * ============================================================ */

#define ICMP_HDR_LEN 8
#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8

/* Sends one ICMP echo request to dst_ip. id/seq are the two fields every
 * ping implementation ever written uses to match replies back to
 * requests -- we don't actually track outstanding pings ourselves (no
 * higher layer here needs an answer *delivered* to it, just logged), but
 * we fill them in properly anyway because a ping tool on the other end
 * absolutely will care. */
static inline int icmp_send_echo_request(u32 dst_ip, u16 id, u16 seq) {
    u8 pkt[ICMP_HDR_LEN + 32];
    pkt[0] = ICMP_TYPE_ECHO_REQUEST;
    pkt[1] = 0;                       /* code: always 0 for echo */
    net_put16_be(&pkt[2], 0);         /* checksum: filled below */
    net_put16_be(&pkt[4], id);
    net_put16_be(&pkt[6], seq);

    /* Payload: the classic alphabet filler every ping(1) sends, purely
     * so packet captures look familiar to anyone who's ever run ping
     * before. Nothing here reads it back. */
    for (int i = 0; i < 32; i++) pkt[ICMP_HDR_LEN + i] = (u8)('a' + (i % 23));

    u16 csum = net_checksum(pkt, sizeof(pkt));
    net_put16_be(&pkt[2], csum);

    serial_puts("[ICMP] echo request -> ");
    net_log_ip(dst_ip);
    serial_putc('\n');
    return ip_send(dst_ip, IP_PROTO_ICMP, pkt, sizeof(pkt));
}

/* Replies to an incoming echo request: same identifier, sequence, and
 * payload bytes come back verbatim (that's the entire point -- the
 * payload is what the far end checks to confirm nothing got mangled in
 * transit), just with the type flipped from request to reply and the
 * checksum redone to match. */
static inline void icmp_send_echo_reply(u32 dst_ip, const u8 *req, u16 req_len) {
    if (req_len > NET_BUF_SIZE - IP_HDR_LEN) return; /* absurdly large ping, decline politely by ignoring it */
    u8 pkt[NET_BUF_SIZE];
    for (u16 i = 0; i < req_len; i++) pkt[i] = req[i];
    pkt[0] = ICMP_TYPE_ECHO_REPLY;
    /* code, id, seq, and payload are already correct from the copy above */
    net_put16_be(&pkt[2], 0);
    u16 csum = net_checksum(pkt, req_len);
    net_put16_be(&pkt[2], csum);

    ip_send(dst_ip, IP_PROTO_ICMP, pkt, req_len);
}

/* Called by the IP dispatcher for every incoming datagram whose
 * protocol field says ICMP. Handles echo request (reply to it) and logs
 * echo reply (proof our own pings are landing); anything else in ICMP's
 * type space is acknowledged in the log and otherwise ignored. */
static inline void icmp_handle_packet(const ip_packet_t *ip) {
    if (ip->payload_len < ICMP_HDR_LEN) return;
    u8 type = ip->payload[0];

    if (type == ICMP_TYPE_ECHO_REQUEST) {
        serial_puts("[ICMP] echo request from ");
        net_log_ip(ip->src_ip);
        serial_puts(", replying\n");
        icmp_send_echo_reply(ip->src_ip, ip->payload, ip->payload_len);
    } else if (type == ICMP_TYPE_ECHO_REPLY) {
        u16 id  = net_get16_be(&ip->payload[4]);
        u16 seq = net_get16_be(&ip->payload[6]);
        serial_puts("[ICMP] echo reply from ");
        net_log_ip(ip->src_ip);
        serial_puts(" id=");
        serial_put_dec(id);
        serial_puts(" seq=");
        serial_put_dec(seq);
        serial_putc('\n');
    } else {
        serial_puts("[ICMP] unhandled type=");
        serial_put_dec(type);
        serial_putc('\n');
    }
}

#endif
