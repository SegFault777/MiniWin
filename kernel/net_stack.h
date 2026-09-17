#ifndef NET_STACK_H
#define NET_STACK_H
#include "io.h"
#include "nic.h"
#include "net.h"
#include "arp.h"
#include "ip.h"
#include "icmp.h"
#include "udp.h"
#include "dns.h"
#include "dhcp.h"
#include "tcp.h"
#include "serial.h"

/* ============================================================
 * net_stack.h -- the front door. Everything above the NIC drivers
 * (ARP, IP, ICMP, UDP, DHCP, and now TCP) gets wired together here into
 * one coherent stack with exactly two entry points the rest of the
 * kernel needs to know about:
 *
 *   net_stack_init()  -- call once, after a NIC driver has come up
 *   net_stack_poll()  -- call once per main-loop iteration, forever
 *
 * Nothing outside this file needs to know that ARP and IP and UDP and
 * TCP are even separate layers; that's an implementation detail this
 * header is built specifically to hide.
 * ============================================================ */

/* Brings the whole stack up: resets protocol state, registers the
 * built-in UDP listeners, and kicks off DHCP so we get a real address
 * from whatever network we're plugged into (QEMU SLIRP today, a real
 * router's DHCP server tomorrow -- the whole point of doing this
 * instead of hardcoding 10.0.2.15). */
static inline void net_stack_init(void) {
    arp_cache_init();
    udp_init();
    net_cfg.ready = 0; /* not until DHCP says otherwise */
    tcp_conn.state = TCP_CLOSED; /* only one connection ever exists; start it idle */
    dns_init(); /* registers the DNS UDP listener; DHCP hasn't handed us
                * a dns_ip yet at this point, but that's fine -- nothing
                * calls dns_resolve() until well after DHCP finishes */
    /* Wires ARP's "an address just resolved" event to IP's one-slot
     * pending-packet queue, so a packet delayed by an ARP miss gets
     * sent the instant the reply lands instead of waiting for whichever
     * higher layer happens to retry next (see ip_flush_pending() in
     * ip.h and the comment on arp_resolved_cb above for why this is a
     * callback instead of a direct call). */
    arp_set_resolved_callback(ip_flush_pending);
    dhcp_start();
}

/* One incoming Ethernet frame, sorted to whichever layer understands
 * it. ARP gets first look (it's not IP-encapsulated, so it has to be
 * handled separately); everything else goes through ip_parse() and
 * then a second dispatch by IP protocol number. */
static inline void net_stack_handle_frame(const u8 *frame, u16 len) {
    if (arp_handle_frame(frame, len)) return;

    if (len < ETH_HDR_LEN) return;
    u16 ethertype = net_get16_be(&frame[12]);
    if (ethertype != ETHERTYPE_IPV4) return; /* not ARP, not IPv4 -- not a protocol we speak */

    ip_packet_t ip;
    if (!ip_parse(frame + ETH_HDR_LEN, (u16)(len - ETH_HDR_LEN), &ip)) return;

    /* Loose but sufficient filter: accept anything addressed to us or
     * broadcast. A real stack also cares about multicast; nothing in
     * this OS ever will, so that's not implemented. */
    if (net_cfg.ready && ip.dst_ip != net_cfg.my_ip && ip.dst_ip != NET_IP4_BROADCAST) return;

    switch (ip.proto) {
        case IP_PROTO_ICMP: icmp_handle_packet(&ip); break;
        case IP_PROTO_UDP:  udp_handle_packet(&ip);  break;
        case IP_PROTO_TCP:  tcp_handle_packet(&ip);  break;
        default:
            /* some protocol we don't implement -- correct behavior is
             * silence, same as udp_handle_packet does for unclaimed
             * ports */
            break;
    }
}

/* Call once per main-loop iteration. Drains every frame currently
 * sitting in the NIC's receive buffer (there may be more than one per
 * iteration under load) and hands each to the dispatcher above. */
static inline void net_stack_poll(void) {
    net_stack_tick();
    if (!nic.present) return;
    u8 buf[NET_BUF_SIZE];
    u16 len;
    /* Drain the whole queue, not just one frame -- if DHCP's OFFER and
     * an unrelated ARP probe both arrived between two poll() calls, both
     * deserve to be handled in the same iteration rather than the
     * second one waiting an entire frame's worth of everything-else. */
    while (nic.recv(buf, sizeof(buf), &len)) {
        net_stack_handle_frame(buf, len);
    }
    tcp_poll_retransmit();
    dns_poll();
}

#endif
