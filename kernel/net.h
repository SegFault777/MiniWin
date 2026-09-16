#ifndef NET_H
#define NET_H
#include "io.h"
#include "nic.h"
#include "serial.h"

/* ============================================================
 * net.h -- the shared toolbox every layer above the NIC drivers reaches
 * into. Endianness helpers, the one-and-only Internet checksum algorithm
 * (used by IP, ICMP, UDP, and TCP alike -- it really is that same dumb
 * arithmetic everywhere), and the handful of constants nobody wants to
 * type twice.
 *
 * A word on "no dependencies": there is no <arpa/inet.h> here, no
 * <sys/socket.h>, no BSD sockets ghost haunting this file. Every byte
 * this stack ever sends is placed by hand, big-endian, because that's
 * what the wire actually wants regardless of what your CPU prefers to
 * think about numbers.
 * ============================================================ */

typedef unsigned long long u64;

/* --- Endianness: x86 is little-endian, the network is big-endian, and
 * the two have been silently judging each other since 1981. These
 * helpers are the peace treaty. --- */
static inline u16 net_htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 net_ntohs(u16 v) { return net_htons(v); } /* it's its own inverse, how tidy */
static inline u32 net_htonl(u32 v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8)  |
           ((v & 0x00FF0000u) >> 8)  |
           ((v & 0xFF000000u) >> 24);
}
static inline u32 net_ntohl(u32 v) { return net_htonl(v); }

static inline void net_put16_be(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)(v & 0xFF); }
static inline void net_put32_be(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)(v & 0xFF);
}
static inline u16 net_get16_be(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static inline u32 net_get32_be(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

#define NET_IP4(a,b,c,d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))
#define NET_IP4_BROADCAST NET_IP4(255,255,255,255)

/* Prints an IP address to the serial log as dotted-quad, because staring
 * at a raw u32 and doing hex-to-decimal in your head is not a skill
 * worth practicing at 2 AM. */
static inline void net_log_ip(u32 ip) {
    serial_put_dec((ip >> 24) & 0xFF); serial_putc('.');
    serial_put_dec((ip >> 16) & 0xFF); serial_putc('.');
    serial_put_dec((ip >> 8)  & 0xFF); serial_putc('.');
    serial_put_dec(ip & 0xFF);
}

/* --- The Internet Checksum: one's-complement sum of 16-bit words, then
 * one's-complement the result. Every protocol above Ethernet reinvents
 * this exact wheel (IP, ICMP, UDP, TCP), presumably because in 1980
 * nobody wanted to `#include` anything either. We at least get to share
 * one implementation across all four.
 *
 * `data` is summed as big-endian 16-bit words starting at an even
 * offset; `len` may be odd (the last byte is padded with zero). This is
 * the raw summing primitive -- callers that need a pseudo-header (UDP,
 * TCP) fold it in via net_checksum_add() before finishing with
 * net_checksum_finish(). --- */
static inline u32 net_checksum_add(u32 sum, const u8 *data, u16 len) {
    while (len > 1) {
        sum += ((u32)data[0] << 8) | data[1];
        data += 2;
        len -= 2;
    }
    if (len == 1) {
        sum += ((u32)data[0] << 8); /* odd trailing byte, high half only */
    }
    return sum;
}
static inline u16 net_checksum_finish(u32 sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum & 0xFFFF);
}
static inline u16 net_checksum(const u8 *data, u16 len) {
    return net_checksum_finish(net_checksum_add(0, data, len));
}

/* Ethertypes and IP protocol numbers -- the wire's little name tags. */
#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806
#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17

#define ETH_HDR_LEN 14
#define ETH_MTU     1500  /* the classic number; nobody here is doing jumbo frames */

/* Every layer builds its packets in one shared scratch buffer -- no
 * malloc exists in this kernel (there is no heap at all, on purpose),
 * so "allocating a packet" means "borrowing a slice of this array for a
 * few microseconds." Sized for one full Ethernet frame with room to
 * spare. */
#define NET_BUF_SIZE 1600

/* Global "do we even have a network" state, filled in once DHCP (or a
 * static fallback) decides what our identity on this Ethernet segment
 * is. Everything above ARP reads this instead of re-deriving it. */
typedef struct {
    u32 my_ip;
    u32 netmask;
    u32 gateway_ip;
    u32 dns_ip;
    int ready; /* 1 once my_ip is something other than 0.0.0.0 */
} net_config_t;

static net_config_t net_cfg = { 0, 0, 0, 0, 0 };

/* A coarse "how much time has passed" counter for anything in the
 * network stack that needs to reason about timeouts -- TCP's
 * retransmission timer, mainly. This is *not* wall-clock time (see
 * rtc.h for that); it's just net_stack_tick() being called once per
 * main-loop iteration, same spirit as the GUI's own `tick` variable in
 * kmain() that drives double-click detection. Deliberately kept as its
 * own counter instead of reaching into kmain()'s local `tick` -- the
 * network stack shouldn't need to know the GUI loop's internal variable
 * names, and a loop-iteration counter is honestly all TCP needs here;
 * this kernel isn't calibrating retransmission timeouts to real
 * milliseconds, just giving up on a SYN that's been unanswered for
 * "clearly too many iterations now." */
static u32 net_ticks = 0;
static inline void net_stack_tick(void) { net_ticks++; }

#endif
