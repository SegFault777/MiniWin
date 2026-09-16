#ifndef UDP_H
#define UDP_H
#include "io.h"
#include "net.h"
#include "ip.h"
#include "serial.h"

/* ============================================================
 * udp.h -- UDP: IP's checksum with a port number stapled on. The entire
 * appeal of this protocol is that it promises you almost nothing
 * (no ordering, no retransmission, no connection setup) and therefore
 * needs almost no code to implement. DHCP lives here. So would DNS, if
 * this kernel ever grows a resolver.
 *
 * Callers register interest in a local port with udp_listen(); incoming
 * datagrams for a registered port get handed to that port's callback.
 * One listener per port, because nothing on this single-user hobby OS
 * needs two things fighting over the same socket.
 * ============================================================ */

#define UDP_HDR_LEN 8
#define UDP_MAX_LISTENERS 4   /* DHCP client + room for whatever comes next (DNS, say) */

typedef void (*udp_handler_t)(u32 src_ip, u16 src_port, const u8 *data, u16 len);

typedef struct {
    u16 port;
    udp_handler_t handler;
    int active;
} udp_listener_t;

static udp_listener_t udp_listeners[UDP_MAX_LISTENERS];
static int udp_listeners_initialized = 0;

static inline void udp_init(void) {
    for (int i = 0; i < UDP_MAX_LISTENERS; i++) udp_listeners[i].active = 0;
    udp_listeners_initialized = 1;
}

/* Registers `handler` to be called for every UDP datagram arriving on
 * `port`. Returns 1 on success, 0 if the listener table is full (which,
 * with 4 slots and this kernel's needs, would be a surprising day). */
static inline int udp_listen(u16 port, udp_handler_t handler) {
    if (!udp_listeners_initialized) udp_init();
    for (int i = 0; i < UDP_MAX_LISTENERS; i++) {
        if (!udp_listeners[i].active) {
            udp_listeners[i].port = port;
            udp_listeners[i].handler = handler;
            udp_listeners[i].active = 1;
            return 1;
        }
    }
    return 0;
}

/* Builds and sends one UDP datagram. UDP's checksum is technically
 * optional over IPv4 (unlike IPv6, where skipping it is a protocol
 * violation), but "technically optional" is how corrupted packets sail
 * through undetected, so we compute it properly every time -- pseudo-
 * header and all. */
static inline int udp_send(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port,
                            const u8 *payload, u16 payload_len) {
    u8 pkt[NET_BUF_SIZE];
    if ((u32)(UDP_HDR_LEN + payload_len) > sizeof(pkt)) return 0;

    net_put16_be(&pkt[0], src_port);
    net_put16_be(&pkt[2], dst_port);
    net_put16_be(&pkt[4], (u16)(UDP_HDR_LEN + payload_len));
    net_put16_be(&pkt[6], 0); /* checksum: filled below, pseudo-header and all */
    for (u16 i = 0; i < payload_len; i++) pkt[UDP_HDR_LEN + i] = payload[i];

    u16 udp_len = (u16)(UDP_HDR_LEN + payload_len);

    /* The pseudo-header: eleven bytes that are never actually
     * transmitted but get summed into the checksum anyway, because the
     * designers of UDP wanted the checksum to also silently vouch for
     * "yes, this really was addressed to you" at the IP level. Every
     * TCP/UDP implementation on Earth carries this same odd little
     * ritual. */
    u8 pseudo[12];
    net_put32_be(&pseudo[0], src_ip);
    net_put32_be(&pseudo[4], dst_ip);
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_UDP;
    net_put16_be(&pseudo[10], udp_len);

    u32 sum = net_checksum_add(0, pseudo, sizeof(pseudo));
    sum = net_checksum_add(sum, pkt, udp_len);
    u16 csum = net_checksum_finish(sum);
    if (csum == 0) csum = 0xFFFF; /* 0 is reserved to mean "no checksum"; dodge it */
    net_put16_be(&pkt[6], csum);

    return ip_send(dst_ip, IP_PROTO_UDP, pkt, udp_len);
}

/* Called by the IP dispatcher for every incoming datagram whose
 * protocol field says UDP. Looks up a listener for the destination
 * port and hands it the payload; datagrams for ports nobody registered
 * are quietly dropped, same as any real UDP/IP stack would do (there's
 * no one home, no point complaining about it). */
static inline void udp_handle_packet(const ip_packet_t *ip) {
    if (ip->payload_len < UDP_HDR_LEN) return;
    u16 src_port = net_get16_be(&ip->payload[0]);
    u16 dst_port = net_get16_be(&ip->payload[2]);
    u16 udp_len  = net_get16_be(&ip->payload[4]);
    if (udp_len < UDP_HDR_LEN || udp_len > ip->payload_len) return;

    const u8 *data = ip->payload + UDP_HDR_LEN;
    u16 data_len = (u16)(udp_len - UDP_HDR_LEN);

    if (!udp_listeners_initialized) udp_init();
    for (int i = 0; i < UDP_MAX_LISTENERS; i++) {
        if (udp_listeners[i].active && udp_listeners[i].port == dst_port) {
            udp_listeners[i].handler(ip->src_ip, src_port, data, data_len);
            return;
        }
    }
    /* nobody listening -- silence is the correct response, per RFC 1122 */
}

#endif
