#ifndef DNS_H
#define DNS_H
#include "io.h"
#include "net.h"
#include "udp.h"
#include "serial.h"

/* ============================================================
 * dns.h -- DNS, or: the phone book that finally lets this kernel type
 * "pypi.org" instead of memorizing 151.101.192.223 and hoping the CDN
 * never rebalances. One query type only (A records -- IPv4 addresses),
 * one outstanding query at a time (same single-instance philosophy as
 * arp.h's cache, tcp.h's connection, and http.h's client -- this kernel
 * does one thing at a time, on purpose), sent to whichever DNS server
 * DHCP told us about.
 *
 * No caching, no CNAME-chasing beyond what the server resolves for us
 * on its own (most public resolvers happily flatten a CNAME chain down
 * to the final A record in one answer, which is the only case this
 * client actually needs to handle), no AAAA/IPv6, no retry on timeout
 * -- a lost query just times out and reports failure, same as this
 * kernel's DHCP client already does for its own UDP exchange. A real
 * resolver library this is not; a resolver that turns one hostname into
 * one IPv4 address, reliably, on a network that isn't actively hostile,
 * it is.
 * ============================================================ */

#define DNS_SERVER_PORT 53
#define DNS_CLIENT_PORT 53000   /* fixed, not really "ephemeral" --
                                 * there's only ever one query in flight,
                                 * so there's nothing for it to collide
                                 * with */
#define DNS_TYPE_A   1
#define DNS_CLASS_IN 1
#define DNS_TIMEOUT_TICKS 4000   /* generous but finite -- see
                                  * tcp.h's TCP_RETRANSMIT_TICKS for what
                                  * a "tick" measures here */
#define DNS_MAX_NAME 128

typedef enum {
    DNS_IDLE = 0,
    DNS_QUERYING,
    DNS_RESOLVED,
    DNS_FAILED,
} dns_state_t;

typedef struct {
    dns_state_t state;
    u16 query_id;
    u32 query_tick;
    u32 result_ip;
    char hostname[DNS_MAX_NAME];   /* kept purely so a timeout/response
                                    * can be logged with a human-readable
                                    * "for hostname X" instead of just an
                                    * opaque transaction id */
} dns_client_t;

static dns_client_t dns_client;

/* Writes `host` into DNS's label-sequence wire format at `out`:
 * "pypi.org" becomes 4 'p' 'y' 'p' 'i' 3 'o' 'r' 'g' 0 -- each dot
 * becomes a length byte for the label that follows it, and the whole
 * name ends with a zero-length label. Returns the number of bytes
 * written. No validation of label length limits (63 bytes) or overall
 * name length (255) beyond DNS_MAX_NAME -- every hostname this kernel
 * actually queries is short and hand-typed into its own source, not
 * arbitrary user input from a network. */
static inline u16 dns_encode_name(u8 *out, const char *host) {
    u16 pos = 0;
    u16 label_len_pos = pos++;
    u8 label_len = 0;
    for (u16 i = 0; ; i++) {
        char c = host[i];
        if (c == '.' || c == 0) {
            out[label_len_pos] = label_len;
            if (c == 0) break;
            label_len = 0;
            label_len_pos = pos++;
        } else {
            out[pos++] = (u8)c;
            label_len++;
        }
    }
    out[pos++] = 0; /* root label -- terminates the name */
    return pos;
}

/* Skips one DNS name at `data + offset`, wire-format aware: a plain
 * label sequence (terminated by a zero-length label) or a compression
 * pointer (a byte with its top two bits set, i.e. 0xC0-0xFF, followed
 * by one more byte -- the pair together is an offset elsewhere in the
 * packet where the "real" name lives). We never need to actually
 * follow a compression pointer to reconstruct the name, because this
 * client never needs to print or compare the name in a response --
 * only skip past it to get to the fixed-size fields (TYPE/CLASS/TTL/
 * RDLENGTH/RDATA) that follow. That asymmetry -- full decode on the
 * way out, skip-only on the way in -- is exactly why dns_encode_name()
 * and this function don't share an implementation. Returns the number
 * of bytes consumed from `offset`, or 0 if the name runs off the end of
 * the buffer (malformed/truncated packet). */
static inline u16 dns_skip_name(const u8 *data, u16 len, u16 offset) {
    u16 pos = offset;
    while (pos < len) {
        u8 b = data[pos];
        if ((b & 0xC0) == 0xC0) { /* compression pointer: 2 bytes, done */
            if (pos + 2 > len) return 0;
            return (u16)(pos + 2 - offset);
        }
        if (b == 0) { /* root label: 1 byte, done */
            return (u16)(pos + 1 - offset);
        }
        pos = (u16)(pos + 1 + b); /* skip this label's length-prefixed bytes */
    }
    return 0; /* ran off the end -- truncated packet */
}

/* Fires off an A-record query for `host` (e.g. "pypi.org") to
 * net_cfg.dns_ip. Overwrites any query already in flight, same
 * single-slot philosophy as arp_send_request()'s cache eviction and
 * tcp_connect()'s single connection -- this kernel only ever wants one
 * answer at a time. */
static inline void dns_resolve(const char *host) {
    dns_client.state = DNS_QUERYING;
    dns_client.query_id = (u16)(net_ticks & 0xFFFF); /* good enough --
                                                       * we only ever have
                                                       * one query outstanding,
                                                       * so the id just needs
                                                       * to not be stale from
                                                       * a previous one */
    dns_client.query_tick = net_ticks;
    dns_client.result_ip = 0;
    u16 i = 0;
    for (; host[i] && i < sizeof(dns_client.hostname) - 1; i++) dns_client.hostname[i] = host[i];
    dns_client.hostname[i] = 0;

    u8 pkt[12 + DNS_MAX_NAME + 4];
    net_put16_be(&pkt[0], dns_client.query_id);
    net_put16_be(&pkt[2], 0x0100); /* standard query, recursion desired --
                                    * "please do the actual legwork of
                                    * chasing referrals, that's your job" */
    net_put16_be(&pkt[4], 1);      /* QDCOUNT: one question */
    net_put16_be(&pkt[6], 0);
    net_put16_be(&pkt[8], 0);
    net_put16_be(&pkt[10], 0);

    u16 pos = 12;
    pos = (u16)(pos + dns_encode_name(&pkt[pos], host));
    net_put16_be(&pkt[pos], DNS_TYPE_A); pos += 2;
    net_put16_be(&pkt[pos], DNS_CLASS_IN); pos += 2;

    serial_puts("[DNS] querying ");
    serial_puts(host);
    serial_putc('\n');
    udp_send(net_cfg.my_ip, DNS_CLIENT_PORT, net_cfg.dns_ip, DNS_SERVER_PORT, pkt, pos);
}

/* UDP listener callback for port DNS_CLIENT_PORT -- parses a response,
 * walks past the (echoed) question section, then scans the answer
 * records for the first A/IN one and takes its address. Any record
 * that isn't a 4-byte A/IN answer (a CNAME, an AAAA, whatever) is
 * skipped over by its own RDLENGTH rather than assumed-away, so a
 * response that leads with a CNAME before the real A record still
 * resolves correctly. */
static inline void dns_handle_reply(u32 src_ip, u16 src_port, const u8 *data, u16 len) {
    (void)src_port;
    if (dns_client.state != DNS_QUERYING) return;
    if (src_ip != net_cfg.dns_ip) return; /* not from the server we asked */
    if (len < 12) return;

    u16 id = net_get16_be(&data[0]);
    if (id != dns_client.query_id) return; /* stale or unrelated reply */

    u16 flags = net_get16_be(&data[2]);
    u8 rcode = (u8)(flags & 0x0F);
    u16 qdcount = net_get16_be(&data[4]);
    u16 ancount = net_get16_be(&data[6]);

    if (rcode != 0) {
        serial_puts("[DNS] server returned error code ");
        serial_put_dec(rcode);
        serial_putc('\n');
        dns_client.state = DNS_FAILED;
        return;
    }

    u16 pos = 12;
    for (u16 q = 0; q < qdcount; q++) {
        u16 nlen = dns_skip_name(data, len, pos);
        if (nlen == 0 || pos + nlen + 4 > len) { dns_client.state = DNS_FAILED; return; }
        pos = (u16)(pos + nlen + 4); /* + QTYPE(2) + QCLASS(2) */
    }

    for (u16 a = 0; a < ancount; a++) {
        u16 nlen = dns_skip_name(data, len, pos);
        if (nlen == 0 || pos + nlen + 10 > len) { dns_client.state = DNS_FAILED; return; }
        pos = (u16)(pos + nlen);
        u16 rtype = net_get16_be(&data[pos]);
        u16 rclass = net_get16_be(&data[pos + 2]);
        u16 rdlength = net_get16_be(&data[pos + 8]);
        pos += 10; /* TYPE(2) + CLASS(2) + TTL(4) + RDLENGTH(2) */
        if (pos + rdlength > len) { dns_client.state = DNS_FAILED; return; }

        if (rtype == DNS_TYPE_A && rclass == DNS_CLASS_IN && rdlength == 4) {
            dns_client.result_ip = net_get32_be(&data[pos]);
            dns_client.state = DNS_RESOLVED;
            serial_puts("[DNS] ");
            serial_puts(dns_client.hostname);
            serial_puts(" is at ");
            net_log_ip(dns_client.result_ip);
            serial_putc('\n');
            return;
        }
        pos = (u16)(pos + rdlength); /* not the record we want -- skip its data and keep looking */
    }

    /* Ran out of answers without finding an A record -- e.g. the name
     * only has an AAAA record, or resolves to nothing. */
    serial_puts("[DNS] no A record found for ");
    serial_puts(dns_client.hostname);
    serial_putc('\n');
    dns_client.state = DNS_FAILED;
}

/* Registers the real UDP listener -- called once from net_stack_init(),
 * same timing as dhcp_start() registering port 68. Separate from
 * dns_resolve() itself so a query can be re-fired without re-registering
 * a listener that's already there. */
static inline void dns_init(void) {
    dns_client.state = DNS_IDLE;
    udp_listen(DNS_CLIENT_PORT, dns_handle_reply);
}

/* Call once per main-loop iteration while a query is outstanding (same
 * poll()-style contract as tcp_poll_retransmit()). Flips a query that's
 * waited too long without a reply from QUERYING to FAILED -- there is
 * deliberately no retry, matching this kernel's DHCP client's own
 * "one shot, trust the network" stance. */
static inline void dns_poll(void) {
    if (dns_client.state != DNS_QUERYING) return;
    if (net_ticks - dns_client.query_tick > DNS_TIMEOUT_TICKS) {
        serial_puts("[DNS] timed out waiting for a reply\n");
        dns_client.state = DNS_FAILED;
    }
}

#endif
