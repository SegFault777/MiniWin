#ifndef MEMMAP_H
#define MEMMAP_H
#include "io.h"

/* ============================================================
 * memmap.h -- the ONE place that decides who owns which chunk of
 * physical memory above 1MB. Paging is off in this kernel, so a raw
 * physical address IS a pointer (the same trick vga.h already uses for
 * the LFB and the truecolor backbuffer at 0x400000).
 *
 *   0x000000 - 0x0FFFFF   real-mode leftovers + kernel image (0x10000..)
 *                         + .bss (see build.sh's ceiling check: .bss must
 *                         stay under 0x9FC00 or it walks into the EBDA/VGA
 *                         window)
 *   0x100000 - 0x17FFFF   the kernel STACK (512KB, grows DOWN from 0x180000)
 *   0x180000 - 0x3FFFFF   the NET ARENA (2.5MB) -- every big network buffer,
 *                         including the NICs' DMA buffers (the card's bus-master
 *                         DMA works on any physical address, and .bss was out of room)
 *   0x400000 - 0x52BFFF   vga.h's backbuffer (640*480*4)
 *
 * WHY THE STACK MOVED: it used to start at 0x9FC00 (the top of
 * conventional memory) and grow down towards a .bss that ends at
 * ~0x9B318 -- an 18KB gap. Meanwhile tls_write_record() alone wants
 * 33KB of locals, gcm_decrypt() 16KB, and elliptic-curve math wants
 * more still. That was a stack overflow waiting to happen, quietly
 * scribbling over whichever globals sat at the top of .bss. A stack
 * that lives in its own 512KB slab of extended memory can't collide
 * with anything, and .bss stops being under pressure from it.
 *
 * WHY IT'S SAFE TO USE THIS RANGE: boot/stage2.asm enables A20 before
 * entering protected mode, and vga_verify_memory_safe() already
 * refuses to boot on machines with less than ~9.4MB of RAM, which is
 * comfortably more than the 4MB this file's map tops out at.
 * ============================================================ */

#define MW_STACK_TOP      0x00180000u   /* kentry.asm loads ESP with this
                                         * -- keep the two in sync (the
                                         * assembler can't read a C
                                         * header, so it's a hand-kept
                                         * copy, called out over there) */
#define MW_STACK_BYTES    0x00080000u

/* ---- the net arena: fixed offsets, laid end to end. Adding a buffer
 * means adding a line here (and nowhere else). The static asserts at
 * the bottom catch an arena overflow at COMPILE time. ---- */
#define MW_NETMEM_BASE    0x00180000u
#define MW_NETMEM_END     0x00400000u

#define MW_TCP_RECV_SIZE  0x00008000u   /* 32KB  inbound TCP bytes, not yet read by TLS/HTTP */
#define MW_TCP_SEND_SIZE  0x00005000u   /* 20KB  outbound queue: holds a whole max-size TLS record + slack */
#define MW_TLS_RAW_SIZE   0x00005000u   /* 20KB  raw TLS record bytes, reassembly (16413-byte max record) */
#define MW_TLS_HS_SIZE    0x00008000u   /* 32KB  reassembled handshake bytes (big cert chains) */
#define MW_TLS_APP_SIZE   0x00008000u   /* 32KB  decrypted application data awaiting the HTTP layer */
#define MW_TLS_REC_SIZE   0x00005000u   /* 20KB  scratch for building one outgoing record */
#define MW_HTTP_BODY_SIZE 0x00040000u   /* 256KB a whole fetched page, headers included */
#define MW_E1000_RX_SIZE  0x00010000u   /* 64KB  e1000 receive DMA buffers (32 descriptors x 2048) */
#define MW_E1000_TX_SIZE  0x00004000u   /* 16KB  e1000 transmit DMA buffers (8 x 2048) */
#define MW_RTL_RX_SIZE    0x00009000u   /* 36KB  rtl8139 receive ring (32KB ring + 16 + one frame of WRAP overrun) */
#define MW_RTL_TX_SIZE    0x00001800u   /* 6KB   rtl8139 transmit slots (4 x 1536) */

#define MW_TCP_RECV_ADDR  (MW_NETMEM_BASE)
#define MW_TCP_SEND_ADDR  (MW_TCP_RECV_ADDR + MW_TCP_RECV_SIZE)
#define MW_TLS_RAW_ADDR   (MW_TCP_SEND_ADDR + MW_TCP_SEND_SIZE)
#define MW_TLS_HS_ADDR    (MW_TLS_RAW_ADDR  + MW_TLS_RAW_SIZE)
#define MW_TLS_APP_ADDR   (MW_TLS_HS_ADDR   + MW_TLS_HS_SIZE)
#define MW_TLS_REC_ADDR   (MW_TLS_APP_ADDR  + MW_TLS_APP_SIZE)
#define MW_HTTP_BODY_ADDR (MW_TLS_REC_ADDR  + MW_TLS_REC_SIZE)
#define MW_E1000_RX_ADDR  (MW_HTTP_BODY_ADDR + MW_HTTP_BODY_SIZE)
#define MW_E1000_TX_ADDR  (MW_E1000_RX_ADDR + MW_E1000_RX_SIZE)
#define MW_RTL_RX_ADDR    (MW_E1000_TX_ADDR + MW_E1000_TX_SIZE)
#define MW_RTL_TX_ADDR    (MW_RTL_RX_ADDR   + MW_RTL_RX_SIZE)
#define MW_NETMEM_USED_END (MW_RTL_TX_ADDR + MW_RTL_TX_SIZE)

/* Compile-time proof the arena is big enough (negative array size =
 * build error, no libc's static_assert needed). */
typedef char mw_assert_netmem_fits[(MW_NETMEM_USED_END <= MW_NETMEM_END) ? 1 : -1];

/* ---- the HTML engine's arena (kernel/dom.h, css.h, layout.h, render.h) ----
 * Lives ABOVE the video backbuffer (0x400000 + 640*480*4 = 0x52C000) so it can't collide with the net arena or
 * the stack, and BELOW the program slot at 0x8F0000. vga_verify_memory_safe() already insists on >= ~9.4MB of RAM
 * (0x92C000), so all of it is real memory on any machine that gets this far. Sizes are in ELEMENTS where an
 * element size is implied by the engine's structs -- the asserts below make a struct that outgrows its slot a
 * COMPILE error instead of a corrupted neighbour. */
#define MW_RD_BASE          0x00530000u
#define MW_RD_NODE_MAX      16384u                   /* DOM nodes (32 bytes each) */
#define MW_RD_ATTR_MAX      24576u                   /* kept attributes (8 bytes each) */
#define MW_RD_POOL_SIZE     0x00040000u              /* 256KB text + attribute values + stylesheet copies */
#define MW_RD_CSS_RULE_MAX  3072u                    /* rules (slot: 32 bytes each) */
#define MW_RD_CSS_COMP_MAX  6144u                    /* selector compounds (slot: 40 bytes each) */
#define MW_RD_CSS_DECL_MAX  12288u                   /* declarations (12 bytes each) */
#define MW_RD_ITEM_MAX      24576u                   /* display-list items (32 bytes each) */
#define MW_RD_FRAG_MAX      1024u                    /* line-builder fragments (slot: 24 bytes each) */
#define MW_RD_SCRATCH_SIZE  0x00060000u              /* 384KB bump arena for tables / flex / grid */

#define MW_RD_NODES_ADDR    (MW_RD_BASE)
#define MW_RD_ATTRS_ADDR    (MW_RD_NODES_ADDR  + MW_RD_NODE_MAX * 32u)
#define MW_RD_POOL_ADDR     (MW_RD_ATTRS_ADDR  + MW_RD_ATTR_MAX * 8u)
#define MW_RD_CSS_RULES_ADDR (MW_RD_POOL_ADDR  + MW_RD_POOL_SIZE)
#define MW_RD_CSS_COMPS_ADDR (MW_RD_CSS_RULES_ADDR + MW_RD_CSS_RULE_MAX * 32u)
#define MW_RD_CSS_DECLS_ADDR (MW_RD_CSS_COMPS_ADDR + MW_RD_CSS_COMP_MAX * 40u)
#define MW_RD_ITEMS_ADDR    (MW_RD_CSS_DECLS_ADDR + MW_RD_CSS_DECL_MAX * 12u)
#define MW_RD_FRAGS_ADDR    (MW_RD_ITEMS_ADDR  + MW_RD_ITEM_MAX * 32u)
#define MW_RD_SCRATCH_ADDR  (MW_RD_FRAGS_ADDR  + MW_RD_FRAG_MAX * 24u)
#define MW_RD_END           (MW_RD_SCRATCH_ADDR + MW_RD_SCRATCH_SIZE)

/* ---- the program (.MWP) slot: the syscall table and the code a program is loaded to ----
 * These USED to sit at 0x84C00/0x85000, "comfortably above .bss" -- until .bss grew past 0x85000 (pre-23) and every
 * program launch silently overwrote tls_conn, tcp_conn, dhcp_*, dns_client, net_cfg and the NIC state. A fixed spot
 * in high memory can't be walked into by a growing kernel. Hand-kept copies live in programs/mwp_api.h and
 * kernel/mwp_link.ld (a linker script can't read this header); mwp.h asserts they agree. */
#define MW_MWP_SYSCALL_ADDR 0x008F0000u
#define MW_MWP_LOAD_ADDR    0x008F1000u
#define MW_MWP_END          0x00900000u

/* ---- big kernel statics that used to live in .bss ----
 * .bss sits just above the kernel image and must stay under the stack's guard band (build.sh checks the margin);
 * every KB the engine's own state costs there is a KB the rest of the kernel can't have. So the biggest
 * pure-cache arrays get a fixed home out here instead (they are always initialised before they are read). */
#define MW_MISC_BASE        0x007C0000u
#define MW_ICON_CACHE_ADDR  (MW_MISC_BASE)               /* 5 icons x 32x32 x u32 = 20KB */
#define MW_ICON_CACHE_SIZE  0x00005000u

typedef char mw_assert_rd_fits[(MW_RD_END <= MW_MISC_BASE) ? 1 : -1];
typedef char mw_assert_misc_fits[((MW_MISC_BASE + MW_ICON_CACHE_SIZE) <= MW_MWP_SYSCALL_ADDR) ? 1 : -1];
typedef char mw_assert_rd_above_backbuf[(MW_RD_BASE >= 0x0052C000u) ? 1 : -1];
typedef char mw_assert_mwp_in_ram[(MW_MWP_END <= 0x0092C000u) ? 1 : -1];

#endif
