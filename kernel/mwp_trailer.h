/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_MWP_TRAILER_H
#define MW_MWP_TRAILER_H

/* mwp_trailer.h -- parsing and validating the metadata trailer of a .mwp. Split out of mwp.h so tools/test/host_mwp_test.c can run it
 * on ordinary arrays (under ASan) against files made by tools/mwp_pack.py and against thousands of damaged ones: the whole point of the
 * validation is that NOTHING in the trailer can make the kernel read outside the file. Needs only u8/u16/u32. */

/* The optional metadata trailer tools/mwp_pack.py appends to a .mwp (icon, title, minimum ABI; layout in that file's header and in
 * docs/dev/MWP-PROGRAMS.md). Parses and VALIDATES it against the file's own length: every offset must lie between the end of the program
 * and the trailer, so a damaged or hostile trailer can never make the kernel read outside the file. Returns 1 with *out filled in, 0 if
 * the file has no (valid) trailer -- which is every program built before alpha-11, and they run exactly as before. */
typedef struct { u32 payload_len, icon16_off, icon32_off, title_off, min_abi; u16 flags; } mwp_trailer_t;
static int mwp_trailer_parse(const u8 *img, u32 len, mwp_trailer_t *out) {
    if (len < 33) return 0;
    const u8 *t = img + len - 32;
    if (t[0] != 'M' || t[1] != 'W' || t[2] != 'P' || t[3] != 'X') return 0;
    #define MWP_LE32(o) ((u32)t[o] | ((u32)t[(o) + 1] << 8) | ((u32)t[(o) + 2] << 16) | ((u32)t[(o) + 3] << 24))
    u32 ver = (u32)t[4] | ((u32)t[5] << 8), flags = (u32)t[6] | ((u32)t[7] << 8);
    u32 payload = MWP_LE32(8), i16 = MWP_LE32(12), i32 = MWP_LE32(16), tit = MWP_LE32(20), abi = MWP_LE32(24);
    #undef MWP_LE32
    u32 end = len - 32;
    if (ver != 1 || (flags & ~7u) || payload == 0 || payload > end) return 0;
    if ((flags & 1) && (i16 < payload || i16 > end || end - i16 < 1024u)) return 0;
    if ((flags & 2) && (i32 < payload || i32 > end || end - i32 < 4096u)) return 0;
    if (flags & 4) {                                                 /* the title must end in a NUL inside the file */
        if (tit < payload || tit >= end) return 0;
        u32 k = tit; while (k < end && img[k]) k++;
        if (k >= end) return 0;
    }
    out->payload_len = payload; out->icon16_off = (flags & 1) ? i16 : 0; out->icon32_off = (flags & 2) ? i32 : 0;
    out->title_off = (flags & 4) ? tit : 0; out->min_abi = abi; out->flags = (u16)flags;
    return 1;
}

#endif
