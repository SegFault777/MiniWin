/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LZ_H
#define LZ_H
#include "io.h"

/* lz.h -- a tiny decoder for the standard LZ4 *block* format (token: high nibble = literal count, low nibble =
 * match length - 4, either one extended by 255-run bytes; literals; 16-bit little-endian match offset). It exists
 * so big read-only tables (today: the 245,784-byte Korean font) can ride in the kernel image compressed and be
 * inflated into high memory at boot -- the image has a hard 512KB budget, high memory does not.
 *
 * Hostile-input safe on purpose: every copy is bounds-checked against both buffers, so a damaged table fails the
 * decode instead of scribbling over a neighbour. Returns the number of bytes produced, or 0 on any error
 * (truncated input, offset pointing before the start of the output, output would exceed dstcap).
 * Overlapping matches (offset < length, the classic run-length trick) are copied byte by byte, as the format wants. */
static u32 lz4_block_decode(const u8 *src, u32 srclen, u8 *dst, u32 dstcap) {
    const u8 *s = src, *send = src + srclen;
    u8 *d = dst, *dend = dst + dstcap;
    while (s < send) {
        u32 token = *s++;
        u32 lit = token >> 4;
        if (lit == 15) { u32 b; do { if (s >= send) return 0; b = *s++; lit += b; } while (b == 255); }
        if (lit > (u32)(send - s) || lit > (u32)(dend - d)) return 0;
        while (lit--) *d++ = *s++;
        if (s == send) break;                               /* the last sequence carries literals only */
        if (send - s < 2) return 0;
        u32 off = (u32)s[0] | ((u32)s[1] << 8); s += 2;
        if (off == 0 || off > (u32)(d - dst)) return 0;
        u32 len = (token & 15) + 4;
        if ((token & 15) == 15) { u32 b; do { if (s >= send) return 0; b = *s++; len += b; } while (b == 255); }
        if (len > (u32)(dend - d)) return 0;
        const u8 *m = d - off;
        while (len--) *d++ = *m++;
    }
    return (u32)(d - dst);
}

#endif
