#ifndef NIC_H
#define NIC_H
#include "io.h"

/* ============================================================
 * Common NIC driver interface -- whichever chipset actually gets found
 * on the PCI bus (RTL8139, e1000, ...) fills this one struct in with its
 * own send/recv functions, so nothing above this layer (an ARP
 * responder, an IP stack, MiniWeb, whatever comes next) needs to know
 * or care which physical card is underneath. Only one NIC is supported
 * "active" at a time -- there's no need for more on a single-user
 * hobby OS, and it keeps this dead simple.
 * ============================================================ */

typedef struct {
    int present;      /* a driver successfully found and initialized a card */
    u8  mac[6];
    const char *chip_name; /* e.g. "RTL8139", for the serial log */

    /* Sends one raw Ethernet frame (dst MAC + src MAC + ethertype +
     * payload, no FCS -- the hardware appends that). Returns 1 on
     * success, 0 if the hardware couldn't take it right now. */
    int (*send)(const u8 *frame, u16 len);

    /* Copies the next received raw Ethernet frame into `out` (up to
     * max_len bytes) and sets *out_len, returning 1 -- or returns 0 if
     * nothing has arrived. Non-blocking; callers poll this every frame
     * the same way everything else in this kernel polls hardware. */
    int (*recv)(u8 *out, u16 max_len, u16 *out_len);
} nic_t;

static nic_t nic = { .present = 0 };

#endif
