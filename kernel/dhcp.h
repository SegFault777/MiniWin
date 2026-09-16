#ifndef DHCP_H
#define DHCP_H
#include "io.h"
#include "net.h"
#include "udp.h"
#include "nic.h"
#include "serial.h"

/* ============================================================
 * dhcp.h -- DHCP: the reason this OS can plug into literally any modern
 * network and get an address, instead of hardcoding 10.0.2.15 forever
 * and only ever working inside one specific QEMU invocation. This is
 * the single biggest unlock for "compatible with modern networks" of
 * anything in this file.
 *
 * The dance is four packets, universally remembered by the acronym
 * DORA: Discover (us, shouting "anyone got a spare IP?" to the whole
 * subnet), Offer (a server replying "yes, have this one"), Request (us,
 * confirming "okay, I'll take it" -- said out loud so *other* DHCP
 * servers who also made offers know to withdraw theirs), and Ack (the
 * server confirming the lease is really ours). We implement exactly
 * this happy path -- no lease renewal, no NAK handling, no rebinding.
 * A hobby OS that boots, gets an address, and gets on with its day
 * doesn't need a lease management subsystem.
 * ============================================================ */

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67

#define DHCP_OP_REQUEST  1
#define DHCP_OP_REPLY    2
#define DHCP_HTYPE_ETH   1

#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_ACK      5
#define DHCP_MSG_NAK      6

#define DHCP_MAGIC_COOKIE 0x63825363u  /* the four bytes that mark "yes, options follow,
                                        * this isn't just BOOTP's ghost" -- baked into
                                        * the spec since RFC 1497 and never changing */

#define DHCP_OPT_PAD          0
#define DHCP_OPT_SUBNET_MASK  1
#define DHCP_OPT_ROUTER       3
#define DHCP_OPT_DNS          6
#define DHCP_OPT_REQUESTED_IP 50
#define DHCP_OPT_LEASE_TIME   51
#define DHCP_OPT_MSG_TYPE     53
#define DHCP_OPT_SERVER_ID    54
#define DHCP_OPT_PARAM_REQ    55
#define DHCP_OPT_END          255

typedef enum {
    DHCP_STATE_IDLE,
    DHCP_STATE_DISCOVER_SENT,
    DHCP_STATE_REQUEST_SENT,
    DHCP_STATE_BOUND
} dhcp_state_t;

static dhcp_state_t dhcp_state = DHCP_STATE_IDLE;
static u32 dhcp_offered_ip = 0;
static u32 dhcp_server_id = 0;
static u32 dhcp_xid = 0x1234ABCD; /* transaction ID -- fixed is fine, we only ever run one at a time */

/* A DHCP packet is BOOTP's fixed 236-byte layout (mostly zeroed fields
 * this simple client doesn't use) followed by the magic cookie and then
 * a TLV option list. We build it field-by-field into a flat buffer
 * rather than defining a struct, because several fields are odd sizes
 * (a 16-byte hardware address field for a 6-byte MAC, 64 and 128 byte
 * string fields we never fill) and a packed struct buys us nothing a
 * comment doesn't already explain. */
#define DHCP_FIXED_LEN 236

static inline u16 dhcp_build_packet(u8 *out, u8 msg_type, u32 requested_ip, u32 server_id) {
    for (int i = 0; i < DHCP_FIXED_LEN; i++) out[i] = 0;

    out[0] = DHCP_OP_REQUEST;
    out[1] = DHCP_HTYPE_ETH;
    out[2] = 6;                              /* hardware address length */
    out[3] = 0;                              /* hops */
    net_put32_be(&out[4], dhcp_xid);
    net_put16_be(&out[8], 0);                /* secs elapsed: not tracked, harmless to omit */
    net_put16_be(&out[10], 0x8000);          /* flags: broadcast bit set -- we have no IP yet,
                                               * so the reply has nowhere unicast to go */
    /* ciaddr (12), yiaddr (16), siaddr (20), giaddr (24): all zero, we're not any of those yet */
    for (int i = 0; i < 6; i++) out[28 + i] = nic.mac[i];  /* chaddr: our hardware address */
    /* sname (44..107), file (108..235): unused, left zero */

    u16 pos = DHCP_FIXED_LEN;
    net_put32_be(&out[pos], DHCP_MAGIC_COOKIE); pos += 4;

    out[pos++] = DHCP_OPT_MSG_TYPE; out[pos++] = 1; out[pos++] = msg_type;

    if (requested_ip) {
        out[pos++] = DHCP_OPT_REQUESTED_IP; out[pos++] = 4;
        net_put32_be(&out[pos], requested_ip); pos += 4;
    }
    if (server_id) {
        out[pos++] = DHCP_OPT_SERVER_ID; out[pos++] = 4;
        net_put32_be(&out[pos], server_id); pos += 4;
    }

    /* Parameter request list: "please also tell me the subnet mask,
     * router, and DNS server while we're here" -- a server is free to
     * ignore any of these, but every server we're likely to meet
     * (a home router, QEMU's SLIRP) happily includes them. */
    out[pos++] = DHCP_OPT_PARAM_REQ; out[pos++] = 3;
    out[pos++] = DHCP_OPT_SUBNET_MASK;
    out[pos++] = DHCP_OPT_ROUTER;
    out[pos++] = DHCP_OPT_DNS;

    out[pos++] = DHCP_OPT_END;
    return pos;
}

static inline void dhcp_send_discover(void) {
    u8 pkt[300];
    u16 len = dhcp_build_packet(pkt, DHCP_MSG_DISCOVER, 0, 0);
    serial_puts("[DHCP] -> DISCOVER\n");
    udp_send(0, DHCP_CLIENT_PORT, NET_IP4_BROADCAST, DHCP_SERVER_PORT, pkt, len);
    dhcp_state = DHCP_STATE_DISCOVER_SENT;
}

static inline void dhcp_send_request(u32 requested_ip, u32 server_id) {
    u8 pkt[300];
    u16 len = dhcp_build_packet(pkt, DHCP_MSG_REQUEST, requested_ip, server_id);
    serial_puts("[DHCP] -> REQUEST for ");
    net_log_ip(requested_ip);
    serial_putc('\n');
    /* Still broadcast: other DHCP servers who made competing offers
     * need to see this too, so they know to let their offers expire
     * instead of thinking we ghosted them. */
    udp_send(0, DHCP_CLIENT_PORT, NET_IP4_BROADCAST, DHCP_SERVER_PORT, pkt, len);
    dhcp_state = DHCP_STATE_REQUEST_SENT;
}

/* Walks the TLV option list looking for `want_opt`. Returns 1 and fills
 * out_val (as a big-endian u32, left-padded with zero if the option is
 * shorter than 4 bytes -- fine for the single-byte msg-type option and
 * the 4-byte address options, which are all we ever look for) if found. */
static inline int dhcp_find_option(const u8 *opts, u16 opts_len, u8 want_opt, u32 *out_val) {
    u16 i = 0;
    while (i < opts_len) {
        u8 opt = opts[i++];
        if (opt == DHCP_OPT_END) break;
        if (opt == DHCP_OPT_PAD) continue;   /* pad has no length byte, just skip it */
        if (i >= opts_len) break;
        u8 opt_len = opts[i++];
        if ((u16)(i + opt_len) > opts_len) break;
        if (opt == want_opt) {
            u32 v = 0;
            for (int j = 0; j < opt_len && j < 4; j++) v = (v << 8) | opts[i + j];
            *out_val = v;
            return 1;
        }
        i = (u16)(i + opt_len);
    }
    return 0;
}

/* UDP listener callback -- registered on port 68 (the client port) at
 * startup, so every DHCP server reply lands here regardless of which
 * state we're currently in. */
static inline void dhcp_handle_reply(u32 src_ip, u16 src_port, const u8 *data, u16 len) {
    (void)src_port;
    if (len < DHCP_FIXED_LEN + 4) return;
    if (net_get32_be(&data[DHCP_FIXED_LEN]) != DHCP_MAGIC_COOKIE) return;
    if (net_get32_be(&data[4]) != dhcp_xid) return; /* not our conversation, ignore */

    u32 offered_ip = net_get32_be(&data[16]); /* yiaddr */
    const u8 *opts = data + DHCP_FIXED_LEN + 4;
    u16 opts_len = (u16)(len - DHCP_FIXED_LEN - 4);

    u32 msg_type = 0;
    dhcp_find_option(opts, opts_len, DHCP_OPT_MSG_TYPE, &msg_type);

    if (msg_type == DHCP_MSG_OFFER && dhcp_state == DHCP_STATE_DISCOVER_SENT) {
        u32 server_id = 0;
        dhcp_find_option(opts, opts_len, DHCP_OPT_SERVER_ID, &server_id);
        serial_puts("[DHCP] <- OFFER of ");
        net_log_ip(offered_ip);
        serial_puts(" from server ");
        net_log_ip(server_id);
        serial_putc('\n');
        dhcp_offered_ip = offered_ip;
        dhcp_server_id = server_id;
        dhcp_send_request(offered_ip, server_id);

    } else if (msg_type == DHCP_MSG_ACK && dhcp_state == DHCP_STATE_REQUEST_SENT) {
        u32 mask = 0, router = 0, dns = 0;
        dhcp_find_option(opts, opts_len, DHCP_OPT_SUBNET_MASK, &mask);
        dhcp_find_option(opts, opts_len, DHCP_OPT_ROUTER, &router);
        dhcp_find_option(opts, opts_len, DHCP_OPT_DNS, &dns);

        net_cfg.my_ip = offered_ip ? offered_ip : dhcp_offered_ip;
        net_cfg.netmask = mask;
        net_cfg.gateway_ip = router;
        net_cfg.dns_ip = dns;
        net_cfg.ready = 1;
        dhcp_state = DHCP_STATE_BOUND;

        serial_puts("[DHCP] <- ACK, bound to ");
        net_log_ip(net_cfg.my_ip);
        serial_puts(" mask=");
        net_log_ip(net_cfg.netmask);
        serial_puts(" gw=");
        net_log_ip(net_cfg.gateway_ip);
        serial_puts(" dns=");
        net_log_ip(net_cfg.dns_ip);
        serial_putc('\n');

    } else if (msg_type == DHCP_MSG_NAK) {
        /* Server said no (probably to our REQUEST -- an offer someone
         * else already snapped up). Simplest correct response: start
         * the whole DORA dance over from scratch. */
        serial_puts("[DHCP] <- NAK, restarting\n");
        dhcp_state = DHCP_STATE_IDLE;
    }
    (void)src_ip;
}

/* Kicks off the DHCP conversation. Call once, after the NIC is up and
 * udp_listen() has a chance to register our callback -- see
 * net_stack_init() in net_stack.h for the actual bring-up order. */
static inline void dhcp_start(void) {
    udp_listen(DHCP_CLIENT_PORT, dhcp_handle_reply);
    dhcp_send_discover();
}

/* No timeout/retry logic yet -- if the DISCOVER or REQUEST gets lost,
 * we just sit in that state forever. A real client would set a timer
 * and retransmit; this one trusts QEMU's SLIRP (and most home routers)
 * to answer the first time, which they overwhelmingly do. A fine
 * enhancement for a future session, noted here instead of pretended
 * away. */

#endif
