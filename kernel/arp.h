#ifndef ARP_H
#define ARP_H
#include "io.h"
#include "nic.h"
#include "net.h"
#include "serial.h"

/* ============================================================
 * arp.h -- the Internet's world's smallest phone book: "I know your IP,
 * what's your MAC?" Every layer above this one (IP, and therefore
 * everything above IP) needs an answer to that question before it can
 * put a single Ethernet frame on the wire, because Ethernet doesn't
 * know what an IP address is and never will.
 *
 * This replaces the one-shot, fire-and-forget net_send_arp_request()
 * over in net_diag.h with an actual table that remembers answers,
 * because asking "who has 10.0.2.2?" fresh before every single packet
 * would be, professionally speaking, unhinged.
 * ============================================================ */

#define ARP_CACHE_SIZE   8      /* a hobby OS talking to a home LAN does
                                 * not need a routing-table-sized ARP
                                 * cache; 8 neighbors is generous */
#define ARP_ENTRY_EMPTY  0
#define ARP_ENTRY_PENDING 1     /* request sent, no reply yet */
#define ARP_ENTRY_RESOLVED 2
/* A who-has that gets no answer must not block that neighbor forever: after this many main-loop ticks
 * the PENDING mark stops counting, so the next ip_send() asks again (a single lost request used to mean
 * "never resolves", since a still-pending entry suppresses new requests). */
#define ARP_PENDING_TIMEOUT_TICKS 2000
typedef struct {
    u32 ip;
    u8  mac[6];
    u8  state;
    u16 age;       /* ticks since last confirmed -- not a real clock,
                     * just a "how stale is this" counter incremented
                     * once per main-loop poll */
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static int arp_cache_initialized = 0;

/* IP wants to know the instant an address it was waiting on resolves,
 * so it can flush a packet that's been sitting in its one-slot pending
 * queue -- but ip.h is built on top of arp.h, not the other way around,
 * and this file isn't about to start #including its own dependents just
 * to make one function call. A single optional callback, registered by
 * whoever ends up wanting it, sidesteps the circular #include without
 * pretending ARP and IP don't both need to know about each other here. */
typedef void (*arp_resolved_cb_t)(u32 ip, const u8 mac[6]);
static arp_resolved_cb_t arp_resolved_cb = 0;
static inline void arp_set_resolved_callback(arp_resolved_cb_t cb) { arp_resolved_cb = cb; }

static inline void arp_cache_init(void) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        arp_cache[i].state = ARP_ENTRY_EMPTY;
    }
    arp_cache_initialized = 1;
}

/* Looks up an IP in the cache. Returns 1 and fills mac_out if resolved,
 * 0 otherwise (whether that's "never heard of them" or "asked, still
 * waiting" -- callers that care about the difference can check
 * arp_is_pending()). */
static inline int arp_lookup(u32 ip, u8 mac_out[6]) {
    if (!arp_cache_initialized) arp_cache_init();
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].state == ARP_ENTRY_RESOLVED && arp_cache[i].ip == ip) {
            for (int j = 0; j < 6; j++) mac_out[j] = arp_cache[i].mac[j];
            return 1;
        }
    }
    return 0;
}

/* Index of the cache slot holding an UNANSWERED request for `ip` (answered or not, however old), or -1.
 * Only such a slot may be turned into a resolved entry by an incoming reply -- that is the whole
 * "we asked, they answered" check. */
static inline int arp_find_pending_slot(u32 ip) {
    if (!arp_cache_initialized) arp_cache_init();
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].ip == ip && arp_cache[i].state == ARP_ENTRY_PENDING) return i;
    }
    return -1;
}

/* True while a request for `ip` is out and still within its timeout (see ARP_PENDING_TIMEOUT_TICKS);
 * `age` holds the net_ticks stamp (low 16 bits) of the moment the request went out. */
static inline int arp_is_pending(u32 ip) {
    int i = arp_find_pending_slot(ip);
    if (i < 0) return 0;
    return (u16)((u16)net_ticks - arp_cache[i].age) < ARP_PENDING_TIMEOUT_TICKS;
}

/* Finds a slot to (re)use for `ip`: an existing entry for that IP if one
 * exists, otherwise the first empty slot, otherwise -- since 8 neighbors
 * should never actually fill up on a single-NIC hobby OS -- just stomps
 * slot 0. LRU eviction is a problem for operating systems with more
 * ambition than this one. */
static inline int arp_find_or_alloc_slot(u32 ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].state != ARP_ENTRY_EMPTY && arp_cache[i].ip == ip) return i;
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].state == ARP_ENTRY_EMPTY) return i;
    }
    return 0;
}

/* Sends a raw ARP request: "who has target_ip? tell sender_ip (that's
 * me, at my_mac)." Marks the cache entry PENDING so we don't spam the
 * network with duplicate requests while waiting for the reply. */
static inline void arp_send_request(u32 sender_ip, u32 target_ip) {
    if (!nic.present) return;
    if (!arp_cache_initialized) arp_cache_init();

    u8 f[42];
    for (int i = 0; i < 6; i++) f[i] = 0xFF;              /* dst: broadcast, everyone gets to ignore this */
    for (int i = 0; i < 6; i++) f[6 + i] = nic.mac[i];    /* src: us */
    net_put16_be(&f[12], ETHERTYPE_ARP);

    net_put16_be(&f[14], 1);        /* hardware type: Ethernet */
    net_put16_be(&f[16], ETHERTYPE_IPV4);
    f[18] = 6;                      /* hardware addr len */
    f[19] = 4;                      /* protocol addr len */
    net_put16_be(&f[20], 1);        /* opcode: request */
    for (int i = 0; i < 6; i++) f[22 + i] = nic.mac[i];
    net_put32_be(&f[28], sender_ip);
    for (int i = 0; i < 6; i++) f[32 + i] = 0;            /* target MAC: that's the whole question */
    net_put32_be(&f[38], target_ip);

    int slot = arp_find_or_alloc_slot(target_ip);
    arp_cache[slot].ip = target_ip;
    arp_cache[slot].state = ARP_ENTRY_PENDING;
    arp_cache[slot].age = (u16)net_ticks;   /* request timestamp: see arp_is_pending() */

    serial_puts("[ARP] who-has ");
    net_log_ip(target_ip);
    serial_puts("? sent\n");
    nic.send(f, sizeof(f));
}

/* Sends an ARP reply, i.e. answering someone else's "who-has" -- polite
 * network citizenship demands this even though nothing downstream of us
 * has asked anyone to talk to us yet. */
static inline void arp_send_reply(u32 my_ip, const u8 requester_mac[6], u32 requester_ip) {
    if (!nic.present) return;
    u8 f[42];
    for (int i = 0; i < 6; i++) f[i] = requester_mac[i];  /* dst: whoever asked */
    for (int i = 0; i < 6; i++) f[6 + i] = nic.mac[i];
    net_put16_be(&f[12], ETHERTYPE_ARP);

    net_put16_be(&f[14], 1);
    net_put16_be(&f[16], ETHERTYPE_IPV4);
    f[18] = 6;
    f[19] = 4;
    net_put16_be(&f[20], 2);        /* opcode: reply */
    for (int i = 0; i < 6; i++) f[22 + i] = nic.mac[i];
    net_put32_be(&f[28], my_ip);
    for (int i = 0; i < 6; i++) f[32 + i] = requester_mac[i];
    net_put32_be(&f[38], requester_ip);

    nic.send(f, sizeof(f));
}

/* Feeds one already-received Ethernet frame to the ARP layer. Returns 1
 * if the frame was ARP and got handled (so the caller's dispatcher knows
 * not to also try feeding it to IP), 0 if it wasn't ARP at all.
 *
 * Handles both directions: someone answering a question we asked
 * (learn it into the cache) and someone asking a question about us
 * (answer it, since staying silent would make us look either dead or
 * rude, and we're neither). */
static inline int arp_handle_frame(const u8 *frame, u16 len) {
    if (len < 42) return 0;
    u16 ethertype = net_get16_be(&frame[12]);
    if (ethertype != ETHERTYPE_ARP) return 0;
    if (!arp_cache_initialized) arp_cache_init();

    /* From here on the frame IS ARP (return 1 = "handled, don't offer it to IP"), but whether it
     * changes anything is decided by the checks below. The old code trusted every field and learned the
     * sender's IP->MAC mapping from any ARP packet whatsoever -- a request, a reply nobody asked for, a
     * "gratuitous" announcement -- which is exactly how ARP spoofing redirects a victim's traffic. */
    if (net_get16_be(&frame[14]) != 1 || net_get16_be(&frame[16]) != ETHERTYPE_IPV4 ||
        frame[18] != 6 || frame[19] != 4) return 1;                    /* not Ethernet/IPv4 ARP */

    u16 opcode = net_get16_be(&frame[20]);
    if (opcode != 1 && opcode != 2) return 1;
    u8 sender_mac[6];
    for (int i = 0; i < 6; i++) sender_mac[i] = frame[22 + i];
    u32 sender_ip = net_get32_be(&frame[28]);
    u32 target_ip = net_get32_be(&frame[38]);

    if (sender_ip == 0 || sender_ip == NET_IP4_BROADCAST) return 1;   /* nobody lives at those addresses */
    if (sender_mac[0] & 1) return 1;                                  /* a multicast/broadcast MAC is never a host */
    for (int i = 0; i < 6; i++) if (frame[6 + i] != sender_mac[i]) return 1;   /* Ethernet source != ARP sender: forged or confused */
    if (net_cfg.ready && sender_ip == net_cfg.my_ip) {               /* someone claims OUR address */
        serial_puts("[ARP] ignoring a packet that claims our own IP\n");
        return 1;
    }

    if (opcode == 2) {
        /* A reply is believed only if (a) it is addressed to us -- target IP and MAC are ours -- and
         * (b) it answers a question we actually have outstanding for that very IP. Unsolicited replies
         * (including gratuitous ones) are ignored: they can neither create nor change an entry. */
        if (target_ip != net_cfg.my_ip) return 1;
        for (int i = 0; i < 6; i++) if (frame[32 + i] != nic.mac[i]) return 1;
        int slot = arp_find_pending_slot(sender_ip);
        if (slot < 0) return 1;
        arp_cache[slot].ip = sender_ip;
        for (int i = 0; i < 6; i++) arp_cache[slot].mac[i] = sender_mac[i];
        arp_cache[slot].state = ARP_ENTRY_RESOLVED;
        arp_cache[slot].age = 0;

        serial_puts("[ARP] ");
        net_log_ip(sender_ip);
        serial_puts(" is at ");
        for (int i = 0; i < 6; i++) {
            serial_put_hex8(sender_mac[i]);
            if (i < 5) serial_putc(':');
        }
        serial_putc('\n');
        if (arp_resolved_cb) arp_resolved_cb(sender_ip, sender_mac);
    } else if (net_cfg.ready && target_ip == net_cfg.my_ip) {
        /* Someone asks who-has US -- answer (the reply is built from the request's own sender fields,
         * no cache entry needed for that). Also remember the asker, but conservatively: a NEW entry only
         * in a genuinely free slot, a refresh only if the MAC is the same one we already have. A request
         * is exactly what a spoofer sends, so it must never rewrite a mapping we hold -- a differing MAC
         * for a known IP is a conflict, logged and ignored. */
        int existing = -1, empty = -1;
        for (int i = 0; i < ARP_CACHE_SIZE; i++) {
            if (arp_cache[i].state == ARP_ENTRY_EMPTY) { if (empty < 0) empty = i; }
            else if (arp_cache[i].ip == sender_ip) existing = i;
        }
        if (existing >= 0) {
            int same = 1;
            for (int i = 0; i < 6; i++) if (arp_cache[existing].mac[i] != sender_mac[i]) same = 0;
            if (arp_cache[existing].state == ARP_ENTRY_RESOLVED && !same) {
                serial_puts("[ARP] conflicting MAC for a known IP ignored: ");
                net_log_ip(sender_ip);
                serial_putc('\n');
            }
        } else if (empty >= 0) {
            arp_cache[empty].ip = sender_ip;
            for (int i = 0; i < 6; i++) arp_cache[empty].mac[i] = sender_mac[i];
            arp_cache[empty].state = ARP_ENTRY_RESOLVED;
            arp_cache[empty].age = 0;
        }
        serial_puts("[ARP] replying to who-has-us from ");
        net_log_ip(sender_ip);
        serial_putc('\n');
        arp_send_reply(net_cfg.my_ip, sender_mac, sender_ip);
    }
    return 1;
}

#endif
