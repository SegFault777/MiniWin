/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_MWP_META_H
#define MW_MWP_META_H

/* mwp_meta.h -- what the kernel knows about an installed program beyond its name: the icon and title packed into its file (see
 * mwp_trailer_t in mwp.h and tools/mwp_pack.py). Read lazily, the first time a program's icon is wanted (desktop, title bar, task bar,
 * MiniFiles), converted to the 0xAARRGGBB the drawing code uses, kept in heap blocks (mm.h), and dropped when the slot's file changes or
 * goes away. A program without a trailer simply has no meta: callers fall back to the generic program icon and the file name.
 * Needs: fs.h (prog_tab, prog_load_slot), mm.h, mwp.h. Not re-entrant, like everything else here. */

typedef struct { u32 ino, len; u8 state; u32 *i16, *i32; char title[24]; } mwp_meta_t;
static mwp_meta_t mwp_meta[PROG_MAX_SLOTS];             /* state: 0 not read yet, 1 read (with or without a trailer) */

static void mwp_meta_drop(int slot) {
    mwp_meta_t *m = &mwp_meta[slot];
    if (m->i16) kfree(m->i16);
    if (m->i32) kfree(m->i32);
    m->i16 = m->i32 = 0; m->title[0] = 0; m->state = 0; m->ino = m->len = 0;
}
static u32 *mwp_rgba_to_argb(const u8 *p, u32 pixels) {
    u32 *o = (u32 *)kmalloc(pixels * 4u);
    if (!o) return 0;
    for (u32 k = 0; k < pixels; k++) o[k] = ((u32)p[k * 4 + 3] << 24) | ((u32)p[k * 4] << 16) | ((u32)p[k * 4 + 1] << 8) | (u32)p[k * 4 + 2];
    return o;
}
/* Makes sure slot's meta is current. */
static void mwp_meta_ensure(int slot) {
    if (slot < 0 || slot >= PROG_MAX_SLOTS) return;
    mwp_meta_t *m = &mwp_meta[slot];
    if (!prog_tab[slot].used) { if (m->state || m->i16 || m->i32) mwp_meta_drop(slot); return; }
    if (m->state && m->ino == prog_tab[slot].ino && m->len == prog_tab[slot].len) return;       /* still the same file */
    mwp_meta_drop(slot);
    u32 len = prog_tab[slot].len;
    if (len < 33 || len > PROG_MAX_BYTES) { m->state = 1; m->ino = prog_tab[slot].ino; m->len = len; return; }
    u8 *img = (u8 *)kmalloc(len);
    if (!img) return;                                                                          /* out of memory: try again next time */
    u32 got = prog_load_slot(slot, img, len);
    m->state = 1; m->ino = prog_tab[slot].ino; m->len = len;
    mwp_trailer_t tr;
    if (got == len && mwp_trailer_parse(img, got, &tr)) {
        if (tr.icon16_off) m->i16 = mwp_rgba_to_argb(img + tr.icon16_off, 256);
        if (tr.icon32_off) m->i32 = mwp_rgba_to_argb(img + tr.icon32_off, 1024);
        if (tr.title_off) { int n = 0; while (img[tr.title_off + n] && n < 23) { m->title[n] = (char)img[tr.title_off + n]; n++; } m->title[n] = 0; }
    }
    kfree(img);
}
static const u32 *mwp_icon16_of(int slot) { mwp_meta_ensure(slot); return (slot >= 0 && slot < PROG_MAX_SLOTS) ? mwp_meta[slot].i16 : 0; }
static const u32 *mwp_icon32_of(int slot) { mwp_meta_ensure(slot); return (slot >= 0 && slot < PROG_MAX_SLOTS) ? mwp_meta[slot].i32 : 0; }
/* The program's own title ("MiniCalc"), or its file name when it has none. */
static const char *mwp_title_of(int slot) {
    mwp_meta_ensure(slot);
    if (slot >= 0 && slot < PROG_MAX_SLOTS && mwp_meta[slot].title[0]) return mwp_meta[slot].title;
    return (slot >= 0 && slot < PROG_MAX_SLOTS) ? prog_names[slot] : "";
}

#endif
