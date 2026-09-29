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
 *   0x180000 - 0x3FFFFF   the NET ARENA (2.5MB) -- every big network buffer
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

#define MW_TCP_RECV_ADDR  (MW_NETMEM_BASE)
#define MW_TCP_SEND_ADDR  (MW_TCP_RECV_ADDR + MW_TCP_RECV_SIZE)
#define MW_TLS_RAW_ADDR   (MW_TCP_SEND_ADDR + MW_TCP_SEND_SIZE)
#define MW_TLS_HS_ADDR    (MW_TLS_RAW_ADDR  + MW_TLS_RAW_SIZE)
#define MW_TLS_APP_ADDR   (MW_TLS_HS_ADDR   + MW_TLS_HS_SIZE)
#define MW_TLS_REC_ADDR   (MW_TLS_APP_ADDR  + MW_TLS_APP_SIZE)
#define MW_HTTP_BODY_ADDR (MW_TLS_REC_ADDR  + MW_TLS_REC_SIZE)
#define MW_NETMEM_USED_END (MW_HTTP_BODY_ADDR + MW_HTTP_BODY_SIZE)

/* Compile-time proof the arena is big enough (negative array size =
 * build error, no libc's static_assert needed). */
typedef char mw_assert_netmem_fits[(MW_NETMEM_USED_END <= MW_NETMEM_END) ? 1 : -1];

#endif
