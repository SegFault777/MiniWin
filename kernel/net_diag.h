#ifndef NET_DIAG_H
#define NET_DIAG_H
#include "io.h"
#include "nic.h"
#include "serial.h"

/* ============================================================
 * Driver bring-up harness -- NOT a network stack. This exists purely to
 * prove, with a real protocol exchange instead of a hopeful comment,
 * that a NIC driver's send() and recv() both actually work: build one
 * genuine ARP request by hand, fire it at QEMU's SLIRP gateway, and log
 * whatever comes back over serial. When a real ARP/IP/TCP stack gets
 * built, this harness is what it replaces -- it is deliberately kept
 * tiny and throwaway.
 * ============================================================ */

static inline void net_put16_be(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)(v & 0xFF); }
static inline void net_put32_be(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)(v & 0xFF);
}

/* Builds and sends one ARP request: "who has target_ip? tell sender_ip".
 * sender_ip/target_ip are plain u32s built from four octets by the
 * caller via NET_IP4() below, for readability at the call site. */
#define NET_IP4(a,b,c,d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

static void net_send_arp_request(u32 sender_ip, u32 target_ip) {
    if (!nic.present) return;
    u8 f[42];

    for (int i = 0; i < 6; i++) f[i] = 0xFF;              /* dst: broadcast */
    for (int i = 0; i < 6; i++) f[6 + i] = nic.mac[i];     /* src: us */
    net_put16_be(&f[12], 0x0806);                          /* ethertype: ARP */

    net_put16_be(&f[14], 1);        /* hardware type: Ethernet */
    net_put16_be(&f[16], 0x0800);   /* protocol type: IPv4 */
    f[18] = 6;                      /* hardware addr len */
    f[19] = 4;                      /* protocol addr len */
    net_put16_be(&f[20], 1);        /* opcode: request */
    for (int i = 0; i < 6; i++) f[22 + i] = nic.mac[i];    /* sender MAC */
    net_put32_be(&f[28], sender_ip);
    for (int i = 0; i < 6; i++) f[32 + i] = 0;             /* target MAC: unknown */
    net_put32_be(&f[38], target_ip);

    serial_puts("[ARP] sending request\n");
    nic.send(f, sizeof(f));
}

/* Call every main-loop iteration once a NIC is present. Logs anything
 * that arrives; specifically recognizes an ARP reply so the "does RX
 * genuinely work" question has an unambiguous yes/no answer in the
 * serial log instead of just a hex dump to eyeball. */
static void net_diag_poll(void) {
    if (!nic.present) return;
    u8 buf[1600];
    u16 len;
    if (!nic.recv(buf, sizeof(buf), &len)) return;

    serial_puts("[RX] ");
    serial_put_hex16(len);
    serial_puts(" bytes, ethertype=");
    u16 ethertype = (u16)((buf[12] << 8) | buf[13]);
    serial_put_hex16(ethertype);

    if (ethertype == 0x0806 && len >= 42) {
        u16 opcode = (u16)((buf[20] << 8) | buf[21]);
        if (opcode == 2) { /* ARP reply */
            serial_puts(" ARP REPLY from ");
            for (int i = 0; i < 6; i++) {
                serial_put_hex8(buf[22 + i]);
                if (i < 5) serial_putc(':');
            }
        }
    }
    serial_putc('\n');
}

#endif
