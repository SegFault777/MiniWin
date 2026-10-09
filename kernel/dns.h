#ifndef DNS_H
#define DNS_H
#include "io.h"
#include "net.h"
#include "udp.h"
#include "serial.h"
#include "netclock.h"

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
#define DNS_TYPE_CNAME 5
#define DNS_CLASS_IN 1
/* TIMEOUTS ARE IN REAL TIME, not loop iterations. The original gave up after 4000 "ticks" -- but a
 * tick is one pass of the main loop, whose speed depends entirely on the machine: ~45 passes/second
 * under software-emulated QEMU (so 4000 ticks was a generous ~90 seconds there), but thousands per
 * second under hardware-accelerated QEMU/VirtualBox/real hardware (so the same 4000 ticks became
 * ~1-4 seconds, and a host resolver with a cold cache could easily take longer -- "DNS Lookup
 * Failed" on a perfectly working network). Each attempt now waits DNS_ATTEMPT_SECONDS of RTC time,
 * and a lookup makes up to DNS_MAX_ATTEMPTS attempts. The RTC has one-second resolution, so a
 * "3 second" wait really fires 2-3 seconds in. DNS_TICK_BACKSTOP covers a machine whose RTC isn't
 * ticking at all, so a lookup can still never hang forever. */
#define DNS_ATTEMPT_SECONDS 3
#define DNS_MAX_ATTEMPTS    4
#define DNS_TICK_BACKSTOP   200000u
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
    u32 query_wall;                 /* RTC seconds-of-day when the current attempt was sent */
    u32 servers[DNS_MAX_ATTEMPTS];  /* who each attempt is sent to */
    int attempt;                    /* 0-based index of the attempt in flight */
    int nattempts;                  /* how many attempts this lookup will make (<= DNS_MAX_ATTEMPTS) */
    char fail[96];                  /* WHY the lookup failed, in words -- shown on the page */
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

/* Decodes the (possibly compressed) domain name at `pos` into lowercase dotted text in `out` (cap bytes,
 * NUL-terminated). Returns the text length, or -1 for anything malformed: a label running past the
 * packet, a reserved label type, a compression pointer that does not point strictly BACKWARD (which also
 * rules out loops), or a name that will not fit. This is what lets a reply's names be COMPARED with what we
 * asked instead of merely skipped over. */
static inline int dns_decode_name(const u8 *data, u16 len, u16 pos, char *out, u16 cap) {
    u16 n = 0, p = pos;
    int hops = 0;
    for (;;) {
        if (p >= len) return -1;
        u8 b = data[p];
        if ((b & 0xC0) == 0xC0) {
            if (p + 1 >= len) return -1;
            u16 ptr = (u16)(((b & 0x3F) << 8) | data[p + 1]);
            if (ptr >= p || ++hops > 16) return -1;      /* must point backward; bounded hops */
            p = ptr;
            continue;
        }
        if (b & 0xC0) return -1;                          /* 0x40 / 0x80 label types are reserved */
        if (b == 0) break;
        if ((u16)(p + 1 + b) > len) return -1;
        if (n != 0) { if (n + 1 >= cap) return -1; out[n++] = '.'; }
        if ((u16)(n + b) >= cap) return -1;
        for (u8 i = 0; i < b; i++) {
            char c = (char)data[p + 1 + i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            out[n++] = c;
        }
        p = (u16)(p + 1 + b);
    }
    out[n] = 0;
    return (int)n;
}

/* Case-insensitive name comparison (DNS names are), tolerating one trailing dot on either side. */
static inline int dns_name_eq(const char *a, const char *b) {
    for (;;) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if ((ca == 0 || (ca == '.' && a[1] == 0)) && (cb == 0 || (cb == '.' && b[1] == 0))) return 1;
        if (ca != cb) return 0;
        a++; b++;
    }
}

static inline u32 dns_wall_seconds(void) { return net_wall_seconds(); }

static inline void dns_ip_str(u32 ip, char *out) {   /* dotted quad, `out` >= 16 bytes */
    u32 n = 0;
    for (int sh = 24; sh >= 0; sh -= 8) {
        u32 o = (ip >> sh) & 0xFF;
        char d[4]; int nd = 0;
        do { d[nd++] = (char)('0' + o % 10); o /= 10; } while (o);
        while (nd) out[n++] = d[--nd];
        if (sh) out[n++] = '.';
    }
    out[n] = 0;
}

static inline void dns_fail_append(const char *text) {
    u32 n = 0; while (dns_client.fail[n]) n++;
    for (u32 i = 0; text[i] && n + 1 < sizeof(dns_client.fail); i++) dns_client.fail[n++] = text[i];
    dns_client.fail[n] = 0;
}

/* Sends the query for the CURRENT attempt to that attempt's server. */
static inline void dns_send_attempt(void) {
    u32 server = dns_client.servers[dns_client.attempt];
    dns_client.query_tick = net_ticks;
    dns_client.query_wall = dns_wall_seconds();

    u8 pkt[12 + DNS_MAX_NAME + 4];
    net_put16_be(&pkt[0], dns_client.query_id);
    net_put16_be(&pkt[2], 0x0100);   /* flags: standard query, recursion desired */
    net_put16_be(&pkt[4], 1);        /* one question */
    net_put16_be(&pkt[6], 0);
    net_put16_be(&pkt[8], 0);
    net_put16_be(&pkt[10], 0);

    u16 pos = 12;
    pos = (u16)(pos + dns_encode_name(&pkt[pos], dns_client.hostname));
    net_put16_be(&pkt[pos], DNS_TYPE_A); pos += 2;
    net_put16_be(&pkt[pos], DNS_CLASS_IN); pos += 2;

    serial_puts("[DNS] querying ");
    serial_puts(dns_client.hostname);
    serial_puts(" via ");
    net_log_ip(server);
    serial_puts(" (attempt ");
    serial_put_dec((u32)dns_client.attempt + 1);
    serial_puts(")\n");
    udp_send(net_cfg.my_ip, DNS_CLIENT_PORT, server, DNS_SERVER_PORT, pkt, pos);
}

/* This attempt didn't work out (no reply, or a server-side error): move on to the next server, or give
 * up with `why` as the reason if there are none left. */
static inline void dns_next_attempt_or_fail(const char *why) {
    if (dns_client.attempt + 1 < dns_client.nattempts) {
        dns_client.attempt++;
        dns_send_attempt();
        return;
    }
    dns_client.fail[0] = 0;
    dns_fail_append(why);
    dns_fail_append(" Tried:");
    for (int i = 0; i < dns_client.nattempts; i++) {
        int seen = 0;
        for (int k = 0; k < i; k++) if (dns_client.servers[k] == dns_client.servers[i]) seen = 1;
        if (seen) continue;
        char ip[16]; dns_ip_str(dns_client.servers[i], ip);
        dns_fail_append(" ");
        dns_fail_append(ip);
    }
    serial_puts("[DNS] giving up on ");
    serial_puts(dns_client.hostname);
    serial_puts(": ");
    serial_puts(dns_client.fail);
    serial_putc('\n');
    dns_client.state = DNS_FAILED;
}

/* Starts looking up `host` (e.g. "pypi.org"). The server list: the DHCP-supplied DNS server (twice -- a
 * lost UDP packet or a slow first ARP exchange is the usual reason a first query goes unanswered),
 * then the default gateway (home routers commonly forward DNS), then 8.8.8.8 as a last resort. Overwrites
 * any lookup already in flight (single-slot, like everything else in this stack). */
static inline void dns_resolve(const char *host) {
    dns_client.state = DNS_QUERYING;
    dns_client.query_id = (u16)((net_ticks * 2654435761u) >> 16);   /* just needs to differ from the last lookup's */
    dns_client.result_ip = 0;
    dns_client.fail[0] = 0;
    dns_client.attempt = 0;
    /* candidate order; any 0.0.0.0 (a DHCP lease with no DNS option, or no lease at all) is skipped,
     * because a query "sent to 0.0.0.0" goes nowhere and just burns an attempt */
    u32 want[5] = { net_cfg.dns_ip, net_cfg.dns_ip, net_cfg.gateway_ip, 0x08080808u, 0x01010101u };
    dns_client.nattempts = 0;
    for (int i = 0; i < 5 && dns_client.nattempts < DNS_MAX_ATTEMPTS; i++) {
        if (want[i] == 0) continue;
        dns_client.servers[dns_client.nattempts++] = want[i];
    }
    u16 i = 0;
    for (; host[i] && i < sizeof(dns_client.hostname) - 1; i++) dns_client.hostname[i] = host[i];
    dns_client.hostname[i] = 0;
    dns_send_attempt();
}

/* UDP listener callback for port DNS_CLIENT_PORT -- parses a response,
 * walks past the (echoed) question section, then scans the answer
 * records for the first A/IN one and takes its address. Any record
 * that isn't a 4-byte A/IN answer (a CNAME, an AAAA, whatever) is
 * skipped over by its own RDLENGTH rather than assumed-away, so a
 * response that leads with a CNAME before the real A record still
 * resolves correctly. */
static inline void dns_handle_reply(u32 src_ip, u16 src_port, const u8 *data, u16 len) {
    if (dns_client.state != DNS_QUERYING) return;
    if (src_port != DNS_SERVER_PORT) return;   /* an answer comes FROM port 53 */
    int asked = 0;                         /* only believe a server we have actually asked */
    for (int k = 0; k <= dns_client.attempt && k < dns_client.nattempts; k++) if (dns_client.servers[k] == src_ip) asked = 1;
    if (!asked) return;
    if (len < 12) return;

    u16 id = net_get16_be(&data[0]);
    if (id != dns_client.query_id) return; /* stale or unrelated reply */

    u16 flags = net_get16_be(&data[2]);
    if (!(flags & 0x8000)) return;         /* QR=0: that is a QUERY, not a response */
    if ((flags >> 11) & 0x0F) return;      /* opcode must echo our standard query (0) */
    u8 rcode = (u8)(flags & 0x0F);
    u16 qdcount = net_get16_be(&data[4]);
    u16 ancount = net_get16_be(&data[6]);

    /* Bind the response to OUR question. We sent exactly one -- the hostname, type A, class IN -- and a
     * genuine server echoes it back. Until now the ID was the only thing tying a reply to a query, so any
     * packet carrying the right 16-bit ID was believed no matter what it was an answer about. A reply
     * with no question, several, or a different one is ignored (not "failed": the real answer may still
     * be on its way). A bare NOERROR/NXDOMAIN with no echoed question is not accepted either: NXDOMAIN
     * in particular is a denial of service if a forgery can produce it. */
    u16 pos = 12;
    if (qdcount > 1) return;
    if (qdcount == 1) {
        char qname[256];
        if (dns_decode_name(data, len, pos, qname, sizeof(qname)) < 0) return;
        u16 nlen = dns_skip_name(data, len, pos);
        if (nlen == 0 || pos + nlen + 4 > len) return;
        u16 qtype = net_get16_be(&data[pos + nlen]);
        u16 qclass = net_get16_be(&data[pos + nlen + 2]);
        if (!dns_name_eq(qname, dns_client.hostname) || qtype != DNS_TYPE_A || qclass != DNS_CLASS_IN) return;
        pos = (u16)(pos + nlen + 4);
    } else if (rcode == 0 || rcode == 3) {
        return;
    }

    if (rcode != 0) {
        char ip[16]; dns_ip_str(src_ip, ip);
        serial_puts("[DNS] ");
        serial_puts(ip);
        serial_puts(" returned error code ");
        serial_put_dec(rcode);
        serial_putc('\n');
        if (rcode == 3) {
            /* NXDOMAIN is the authoritative "that name does not exist": asking someone else won't help */
            dns_client.fail[0] = 0;
            dns_fail_append("No such host (server ");
            dns_fail_append(ip);
            dns_fail_append(" says it does not exist).");
            dns_client.state = DNS_FAILED;
        } else {
            /* SERVFAIL / REFUSED / NOTIMP: this server is unwell or unwilling -- try the next one */
            char why[40] = "Server error ";
            u32 n = 13; why[n++] = (char)('0' + (rcode % 10)); why[n] = 0;
            dns_next_attempt_or_fail(why);
        }
        return;
    }

    /* Answers. A record only counts if its OWNER NAME is the name we are following: the queried hostname
     * at first, and after a CNAME (owner == the name so far) the CNAME's target. A response that "answers"
     * with records for some other name -- the classic way to slip an address in next to a plausible
     * header -- therefore resolves nothing. Record boundaries are checked against the datagram as
     * before. */
    char want[256];   /* a full-length (253) CNAME target must fit, hence not DNS_MAX_NAME */
    {   u16 hi = 0; while (dns_client.hostname[hi] && hi < DNS_MAX_NAME) { char c = dns_client.hostname[hi]; want[hi] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; hi++; } want[hi] = 0; }
    int saw_foreign = 0;   /* a record owned by some name that is not on our CNAME chain */
    for (u16 a = 0; a < ancount; a++) {
        u16 nlen = dns_skip_name(data, len, pos);
        if (nlen == 0 || pos + nlen + 10 > len) { dns_client.state = DNS_FAILED; return; }
        char owner[256];
        int owner_ok = dns_decode_name(data, len, pos, owner, sizeof(owner)) >= 0 && dns_name_eq(owner, want);
        if (!owner_ok) saw_foreign = 1;
        pos = (u16)(pos + nlen);
        u16 rtype = net_get16_be(&data[pos]);
        u16 rclass = net_get16_be(&data[pos + 2]);
        u16 rdlength = net_get16_be(&data[pos + 8]);
        pos += 10; /* TYPE(2) + CLASS(2) + TTL(4) + RDLENGTH(2) */
        if (pos + rdlength > len) { dns_client.state = DNS_FAILED; return; }

        if (owner_ok && rtype == DNS_TYPE_CNAME && rclass == DNS_CLASS_IN) {
            char target[256];
            if (dns_decode_name(data, len, pos, target, sizeof(target)) >= 0) {
                u16 ti = 0; while (target[ti] && ti < 255) { want[ti] = target[ti]; ti++; } want[ti] = 0;
            }
        } else if (owner_ok && rtype == DNS_TYPE_A && rclass == DNS_CLASS_IN && rdlength == 4) {
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

    /* Records were present but none belonged to our name: that is not "the name has no address", it is a
     * response that does not answer our question. Ignore it (stay QUERYING) rather than let such a packet
     * end the lookup; the real answer, or the timeout, will. */
    if (saw_foreign) return;

    /* Ran out of answers without finding an A record -- e.g. the name
     * only has an AAAA record, or resolves to nothing. */
    serial_puts("[DNS] no A record found for ");
    serial_puts(dns_client.hostname);
    serial_putc('\n');
    dns_client.fail[0] = 0;
    dns_fail_append("The name exists but has no IPv4 address (A record).");
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
    u32 now = dns_wall_seconds();
    u32 waited = (now + 86400u - dns_client.query_wall) % 86400u;   /* RTC seconds, safe across midnight */
    if (waited >= DNS_ATTEMPT_SECONDS || net_ticks - dns_client.query_tick > DNS_TICK_BACKSTOP) {
        serial_puts("[DNS] no reply within the time limit\n");
        dns_next_attempt_or_fail("No reply from the DNS server.");
    }
}

#endif
