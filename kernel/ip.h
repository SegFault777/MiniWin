#ifndef IP_H
#define IP_H
#include "io.h"
#include "nic.h"
#include "net.h"
#include "arp.h"
#include "serial.h"

/* ============================================================
 * ip.h -- IPv4: the protocol whose entire job is deciding "does this
 * packet need to leave the building, and if so, to whom do I hand it at
 * the loading dock (i.e. which MAC address)." Everything above this
 * layer (ICMP, UDP, TCP) just wants to say "send these bytes to that IP"
 * and trusts IP to sort out Ethernet framing, fragmentation (we don't --
 * see below), and the checksum arithmetic.
 *
 * No fragmentation, no options, no IPv6 (the "v4" in the filename is a
 * promise, not an oversight). This kernel controls both ends of every
 * connection it dreams up and never needs to send a datagram bigger
 * than one Ethernet frame, so reassembling fragments would be pure
 * ceremony -- code written to satisfy a spec nobody here will ever
 * trigger.
 * ============================================================ */

#define IP_HDR_LEN 20   /* no options, ever -- see above */
#define IP_TTL_DEFAULT 64

/* Parsed view of an incoming IPv4 header -- callers get friendly fields
 * instead of poking offsets by hand every time. */
typedef struct {
    u8  version_ihl;
    u8  proto;
    u16 total_len;
    u32 src_ip;
    u32 dst_ip;
    const u8 *payload;
    u16 payload_len;
} ip_packet_t;

/* Builds one IPv4 header directly into `out` (must have >= IP_HDR_LEN
 * bytes). Does NOT touch the payload that follows -- caller writes that
 * in separately, then this function's checksum pass covers only the
 * 20-byte header, exactly per spec (the payload gets its own checksum
 * story in ICMP/UDP/TCP, each of which folds in a pseudo-header --
 * that's their problem, not IP's). */
static inline void ip_build_header(u8 *out, u32 src_ip, u32 dst_ip, u8 proto, u16 payload_len) {
    out[0] = 0x45;                              /* version 4, IHL 5 (x4 = 20 bytes, no options) */
    out[1] = 0x00;                              /* DSCP/ECN: we have no opinion on traffic priority */
    net_put16_be(&out[2], (u16)(IP_HDR_LEN + payload_len));
    net_put16_be(&out[4], 0);                   /* identification: fine at 0, we never fragment
                                                  * so nothing ever needs to match fragments back up */
    net_put16_be(&out[6], 0x4000);              /* flags=DF (don't fragment), frag offset=0 --
                                                  * we're promising the network we'll never make it
                                                  * fragment us, and then never giving it a reason to */
    out[8] = IP_TTL_DEFAULT;
    out[9] = proto;
    net_put16_be(&out[10], 0);                  /* checksum: zeroed before computing, as tradition demands */
    net_put32_be(&out[12], src_ip);
    net_put32_be(&out[16], dst_ip);

    u16 csum = net_checksum(out, IP_HDR_LEN);
    net_put16_be(&out[10], csum);
}

/* Parses an incoming IPv4 header. Returns 1 and fills *pkt on success,
 * 0 if this isn't a well-formed IPv4 header we can use (wrong version,
 * has options we don't parse, truncated, whatever). */
static inline int ip_parse(const u8 *data, u16 len, ip_packet_t *pkt) {
    if (len < IP_HDR_LEN) return 0;
    if ((data[0] >> 4) != 4) return 0;          /* not IPv4 -- not our problem */
    u8 ihl_words = data[0] & 0x0F;
    u16 ihl_bytes = (u16)(ihl_words * 4);
    if (ihl_bytes < IP_HDR_LEN || ihl_bytes > len) return 0;

    pkt->version_ihl = data[0];
    pkt->proto = data[9];
    pkt->total_len = net_get16_be(&data[2]);
    pkt->src_ip = net_get32_be(&data[12]);
    pkt->dst_ip = net_get32_be(&data[16]);

    /* Options (if ihl_bytes > 20) are skipped, not parsed -- we don't
     * originate them and don't need to honor anyone else's. The payload
     * starts right after however many header bytes there actually are. */
    if (pkt->total_len < ihl_bytes || pkt->total_len > len) return 0;
    pkt->payload = data + ihl_bytes;
    pkt->payload_len = (u16)(pkt->total_len - ihl_bytes);
    return 1;
}

/* ARP is asynchronous -- by the time a reply comes back, whoever wanted
 * to send the original packet has long since moved on. Rather than
 * making every single caller of ip_send() implement its own "retry
 * until ARP resolves" logic, we hold exactly one pending packet here
 * and flush it automatically the moment its next-hop resolves. One slot
 * is enough for a single-user hobby OS that isn't trying to blast out a
 * hundred connections at once; a second ip_send() while one is already
 * pending simply overwrites the first (last write wins, same as the
 * ARP cache eviction policy above -- consistency, if nothing else).
 *
 * The payload buffer is capped at 512 bytes, not the full 1566-byte MTU
 * -- ARP misses happen when we're speaking to a *new* neighbor, which in
 * practice means a small ICMP echo, a DHCP negotiation packet, or a
 * short UDP datagram, never a max-size TCP segment (TCP only ever talks
 * to a peer it already ARP-resolved during connection setup). A packet
 * bigger than this still gets sent via ip_send() -- it just skips the
 * queue and relies on the caller to retry, exactly like before this
 * queue existed. Trading "some oversized packets don't auto-flush" for
 * "1KB less BSS eating into the stack's turf" is the right side of that
 * trade on a kernel this size. */
#define IP_PENDING_MAX_PAYLOAD 512
typedef struct {
    int pending;
    u32 dst_ip;
    u8  proto;
    u8  payload[IP_PENDING_MAX_PAYLOAD];
    u16 payload_len;
} ip_pending_t;

static ip_pending_t ip_pending = { 0, 0, 0, {0}, 0 };

/* Decides who the Ethernet frame's destination MAC should actually be:
 * if dst_ip is on our local subnet, ARP for it directly; otherwise ARP
 * for the gateway and let it worry about the rest of the planet. This
 * one-line decision is the entirety of this kernel's routing table. */
static inline u32 ip_next_hop(u32 dst_ip) {
    if (dst_ip == NET_IP4_BROADCAST) return NET_IP4_BROADCAST;
    if (net_cfg.netmask && (dst_ip & net_cfg.netmask) == (net_cfg.my_ip & net_cfg.netmask)) {
        return dst_ip;               /* same subnet, talk to it directly */
    }
    return net_cfg.gateway_ip;       /* somewhere else -- that's the gateway's problem */
}

/* The actual wire-writing half: called only once we already know the
 * destination MAC, no ARP guesswork left to do. */
static inline int ip_send_now(u32 dst_ip, u8 proto, const u8 *payload, u16 payload_len, const u8 dst_mac[6]) {
    u8 frame[NET_BUF_SIZE];
    for (int i = 0; i < 6; i++) frame[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) frame[6 + i] = nic.mac[i];
    net_put16_be(&frame[12], ETHERTYPE_IPV4);

    ip_build_header(&frame[14], net_cfg.my_ip, dst_ip, proto, payload_len);
    for (u16 i = 0; i < payload_len; i++) frame[14 + IP_HDR_LEN + i] = payload[i];

    u16 total = (u16)(ETH_HDR_LEN + IP_HDR_LEN + payload_len);
    return nic.send(frame, total);
}

/* Sends one IPv4 datagram. `payload` (already fully built, including
 * whatever protocol header ICMP/UDP/TCP put on it) gets an IP header
 * prepended and an Ethernet header prepended to that, then goes straight
 * to the NIC -- *if* we already know the next hop's MAC. If we don't,
 * this fires off an ARP request, stashes the packet in the one-slot
 * pending queue above, and returns immediately; ip_flush_pending() (see
 * below, called from the ARP reply path) finishes the job the instant
 * the MAC resolves. Higher layers no longer need to notice any of this
 * happened -- one ip_send() call is now enough, ARP miss or not.
 *
 * Returns 1 if the frame made it to the NIC already, 0 if it's queued
 * (or truly undeliverable -- no NIC, no route, oversized). */
static inline int ip_send(u32 dst_ip, u8 proto, const u8 *payload, u16 payload_len) {
    /* Normally we refuse to send anything until DHCP has handed us a
     * real identity -- but DHCP itself has to send its DISCOVER (and
     * REQUEST) *before* that's true, from 0.0.0.0, straight to the
     * broadcast address. That's the one deliberate exception: no
     * address yet is fine as long as the destination is broadcast,
     * because broadcast is the only address a nobody is allowed to
     * shout at. */
    if (!nic.present) return 0;
    if (!net_cfg.ready && dst_ip != NET_IP4_BROADCAST) return 0;
    if (payload_len > ETH_MTU - IP_HDR_LEN) return 0; /* no fragmentation, see file header */

    u32 next_hop = ip_next_hop(dst_ip);
    u8 dst_mac[6];

    if (next_hop == NET_IP4_BROADCAST) {
        for (int i = 0; i < 6; i++) dst_mac[i] = 0xFF;
        return ip_send_now(dst_ip, proto, payload, payload_len, dst_mac);
    }
    if (arp_lookup(next_hop, dst_mac)) {
        return ip_send_now(dst_ip, proto, payload, payload_len, dst_mac);
    }

    /* Unresolved -- ask ARP, park the packet, come back to it later.
     * The parking spot is smaller than the MTU (see IP_PENDING_MAX_PAYLOAD
     * above); a packet too big to fit there still gets ARP requested but
     * not queued, falling back to "caller retries" -- which in practice
     * only ever affects a jumbo packet aimed at a neighbor we've never
     * spoken to yet, not anything this kernel actually sends today. */
    if (!arp_is_pending(next_hop)) arp_send_request(net_cfg.my_ip, next_hop);
    if (payload_len <= IP_PENDING_MAX_PAYLOAD) {
        ip_pending.pending = 1;
        ip_pending.dst_ip = dst_ip;
        ip_pending.proto = proto;
        for (u16 i = 0; i < payload_len; i++) ip_pending.payload[i] = payload[i];
        ip_pending.payload_len = payload_len;
    }
    return 0;
}

/* Called from the ARP layer whenever a reply resolves an address --
 * if that address is the one our one pending packet was waiting on,
 * send it now and clear the slot. A single-packet queue means a second
 * unresolved send would have already overwritten the first while it
 * waited (see ip_pending_t above), so there is at most ever one packet
 * to flush here, never a backlog to drain. */
static inline void ip_flush_pending(u32 resolved_ip, const u8 mac[6]) {
    if (!ip_pending.pending) return;
    u32 next_hop = ip_next_hop(ip_pending.dst_ip);
    if (next_hop != resolved_ip) return;

    ip_pending.pending = 0; /* clear first: ip_send_now() must never re-enter this slot */
    ip_send_now(ip_pending.dst_ip, ip_pending.proto, ip_pending.payload, ip_pending.payload_len, mac);
}

#endif
