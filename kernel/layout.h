#ifndef LAYOUT_H
#define LAYOUT_H
#include "css.h"

/* ============================================================
 * layout.h -- the back half of MiniWeb's HTML5 engine: a styled tree
 * goes in, a flat DISPLAY LIST of pixel-positioned things to paint
 * comes out (rectangles, text runs, bullets, image placeholders,
 * form controls). render.h paints that list and answers "what is under
 * the mouse"; nothing here touches the screen, so a host-side test can
 * run the whole thing and dump pictures.
 *
 * WHAT IT LAYS OUT:
 *   - block formatting: margins (with sibling and parent/child collapsing),
 *     padding, borders, widths (auto / px / % / min / max / auto-margin
 *     centering), backgrounds, lists with markers, <hr>, <pre>
 *   - inline formatting: a line builder that wraps words, keeps glued
 *     inline pieces together, aligns baselines (sub/sup/top/middle/
 *     bottom too), honours text-align, text-indent, white-space, <br>
 *   - atomic inline boxes: inline-block, <img>, <input>, <button>,
 *     <select>, <textarea>, <svg>, ...  (images are placeholder boxes with
 *     their alt text: this engine fetches one resource per page, the page)
 *   - tables (auto layout with colspan/rowspan, cellspacing/padding,
 *     collapsed borders, captions), flexbox (row/column, wrap, grow/
 *     shrink, justify-content, align-items, gap) and a basic grid
 *     (grid-template-columns with px / fr / % / repeat() / auto-fit)
 *   - <details>/<summary> that actually open and close
 *
 * WHAT IT DOESN'T: floats (a float becomes an inline-block, which is
 * right for the navigation bars floats are used for and wrong for text
 * wrapping around a picture), absolute/fixed positioning (laid out in
 * flow), z-index, transforms, generated content, background images.
 *
 * COORDINATES: document pixels, y grows downward, x from the left edge
 * of the viewport. A fragment of a line is laid out at its own (0,0) and
 * translated into place when the line is finished -- which is also what
 * lets a line re-wrap by simply moving the tail fragments.
 * ============================================================ */

#ifndef RD_ITEMS
#define RD_ITEM_MAX    MW_RD_ITEM_MAX
#define RD_FRAG_MAX    MW_RD_FRAG_MAX
#define RD_SCRATCH_SZ  MW_RD_SCRATCH_SIZE
#define RD_ITEMS   ((rd_item_t *)MW_RD_ITEMS_ADDR)
#define RD_FRAGS   ((rd_frag_t *)MW_RD_FRAGS_ADDR)
#define RD_SCRATCH ((u8 *)MW_RD_SCRATCH_ADDR)
#endif

enum { DL_NONE = 0, DL_RECT, DL_TEXT, DL_BULLET, DL_IMG, DL_CTL };
#define TF_BOLD    1
#define TF_ITALIC  2
#define TF_UNDER   4
#define TF_STRIKE  8
#define TF_UPPER   16
#define TF_LOWER   32
#define TF_CAP     64
#define TF_PRE     128           /* whitespace in the run is literal (no collapsing) */

enum { CTL_TEXT = 1, CTL_PASSWORD, CTL_BUTTON, CTL_CHECK, CTL_RADIO, CTL_SELECT, CTL_TEXTAREA };
enum { BUL_DISC = 1, BUL_CIRCLE, BUL_SQUARE, BUL_RIGHT, BUL_DOWN };

typedef struct {
    u8 kind, flags, cell, aux;
    int y;
    short x, w, h;
    u16 len;
    u32 color, color2;           /* text/rect/bullet colour; text: background behind the run (CSS_NOCOLOR = none) */
    u32 off;                     /* text/alt text: pool offset of the first byte */
    u32 ref;                     /* the <a> this belongs to (link), or the form control's node */
} rd_item_t;                     /* 32 bytes */

typedef struct {
    int item0, item1;            /* the display-list items this fragment owns */
    short x, w, h, asc;          /* x relative to the line's left edge */
    u8 valign, brk, kind, pad;   /* brk: a line may break just before this fragment */
} rd_frag_t;

typedef struct {
    u32 n;
    int full;
    u32 link;                    /* the open <a href> (0 = none): stamped onto every item emitted inside it */
    int mark_item;               /* a list marker waiting for its line, or -1 */
    int mark_cell;
    int doc_h, doc_w;
    u32 canvas_bg;
    u32 scratch_top;
    u32 pool_mark;               /* pool length to restore before each layout (markers are appended there) */
    int depth;
    int last_w;                  /* outer width (margins included) of the table lay_table just finished */
    rd_item_t dummy;
} rd_layout_t;
static rd_layout_t L;

#define RD_MAX_DEPTH 90

/* ---------- display list ---------- */
static inline rd_item_t *dl_new(void) {
    if (L.n >= RD_ITEM_MAX) { L.full = 1; L.dummy.kind = 0; return &L.dummy; }
    rd_item_t *it = &RD_ITEMS[L.n++];
    css_zero(it, sizeof(*it));
    return it;
}
static inline void dl_translate(int i0, int i1, int dx, int dy) {
    for (int i = i0; i < i1 && i < (int)L.n; i++) { RD_ITEMS[i].x = (short)(RD_ITEMS[i].x + dx); RD_ITEMS[i].y += dy; }
}
static inline void dl_rect(int x, int y, int w, int h, u32 color) {
    if (w <= 0 || h <= 0 || color == CSS_NOCOLOR) return;
    rd_item_t *it = dl_new();
    it->kind = DL_RECT; it->x = (short)x; it->y = y; it->w = (short)w; it->h = (short)h; it->color = color; it->ref = L.link;
}

static inline u32 col_light(u32 c) { u32 r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255; return ((r + (255 - r) * 3 / 4) << 16) | ((g + (255 - g) * 3 / 4) << 8) | (b + (255 - b) * 3 / 4); }
static inline u32 col_dark(u32 c) { u32 r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255; return ((r / 2) << 16) | ((g / 2) << 8) | (b / 2); }

/* A box's background and border strips are painted BEFORE its content but can only be sized AFTER it, so the
 * slots are reserved up front (kind 0 = painter ignores them) and filled in once the height is known. */
typedef struct { int base, n; } dl_box_t;
static inline int st_has_border(const css_style_t *st) { return st->bw[0] || st->bw[1] || st->bw[2] || st->bw[3]; }
static inline dl_box_t dl_box_reserve(const css_style_t *st) {
    dl_box_t b; b.base = (int)L.n; b.n = 0;
    int want = (st->bg != CSS_NOCOLOR ? 1 : 0);
    for (int i = 0; i < 4; i++) if (st->bw[i]) want++;
    for (int i = 0; i < want; i++) { rd_item_t *it = dl_new(); it->kind = DL_NONE; }
    b.n = want;
    return b;
}
/* skip_mask: bit i set = don't draw side i (T,R,B,L) -- used by collapsed table borders */
static inline void dl_box_fill(dl_box_t b, int x, int y, int w, int h, const css_style_t *st, int skip_mask) {
    if (b.n == 0 || L.full || b.base + b.n > (int)L.n) return;
    int k = b.base;
    #define SLOT(X, Y, W, H, C) do { if (k < b.base + b.n) { rd_item_t *it = &RD_ITEMS[k++]; if ((W) > 0 && (H) > 0) { it->kind = DL_RECT; it->x = (short)(X); \
        it->y = (Y); it->w = (short)(W); it->h = (short)(H); it->color = (C); it->ref = L.link; } } } while (0)
    if (st->bg != CSS_NOCOLOR) SLOT(x, y, w, h, st->bg);
    int bt = st->bw[0], br = st->bw[1], bb = st->bw[2], bl = st->bw[3];
    u32 c[4];
    for (int i = 0; i < 4; i++) {
        u32 base = st->bcolor[i];
        int style = st->bstyle[i];
        int lightside = (i == 0 || i == 3);             /* top, left */
        if (style == BS_INSET || style == BS_GROOVE) c[i] = lightside ? col_dark(base) : col_light(base);
        else if (style == BS_OUTSET || style == BS_RIDGE) c[i] = lightside ? col_light(base) : col_dark(base);
        else c[i] = base;
    }
    if (bt && !(skip_mask & 1)) SLOT(x, y, w, bt, c[0]); else if (bt) { k++; }
    if (bb && !(skip_mask & 4)) SLOT(x, y + h - bb, w, bb, c[2]); else if (bb) { k++; }
    if (bl && !(skip_mask & 8)) SLOT(x, y + bt, bl, h - bt - bb, c[3]); else if (bl) { k++; }
    if (br && !(skip_mask & 2)) SLOT(x + w - br, y + bt, br, h - bt - bb, c[1]); else if (br) { k++; }
    #undef SLOT
}

/* ---------- vertical margin arithmetic ---------- */
static inline int mcombine(int a, int b) {                 /* CSS margin collapsing: both positive -> max; both negative -> min; else sum */
    if (a >= 0 && b >= 0) return a > b ? a : b;
    if (a < 0 && b < 0) return a < b ? a : b;
    return a + b;
}

/* ---------- the layout cursor of one block container ---------- */
typedef struct { int x, w, y, pend; } lbox_t;

/* ---------- text metrics ---------- */
static inline int st_line_height(const css_style_t *st) {
    int c = st->cell, lh;
    if (st->lh_mode == 1) lh = st->lh;
    else if (st->lh_mode == 2) {
        int px = (st->fpx * CSS_ZOOM_NUM + CSS_ZOOM_DEN / 2) / CSS_ZOOM_DEN;
        lh = (px * st->lh + 50) / 100;
    } else lh = (c * 5 + 3) / 4;
    if (lh < c + 2) lh = c + 2;
    return lh;
}
static inline int st_asc(const css_style_t *st, int lh) { return (lh - st->cell) / 2 + (st->cell * 9) / 11; }

/* number of glyph cells in t[0..n): ASCII = 1 byte per cell, Hangul = 3 bytes per cell */
static inline u32 txt_cells(const u8 *t, u32 n) {
    u32 c = 0;
    for (u32 i = 0; i < n; i++) { if (t[i] >= 0xE0) i += 2; c++; }
    return c;
}

/* ============================================================
 * the line builder
 * ============================================================ */
typedef struct {
    int x0, w;                   /* the line box: left edge (doc px) and width */
    int pen;                     /* where the next fragment goes, relative to x0 */
    int line_y;                  /* top of the line being built (set when its first fragment arrives) */
    int fbase, nf;               /* this builder's slice of the fragment stack */
    int space;                   /* collapsible whitespace is pending before the next thing */
    int first_line;
    int indent;
    int strut_h, strut_asc;
    u8 text_align, active;
    lbox_t *cur;                 /* the block cursor the finished lines advance */
} lb_t;
static lb_t LB;

#define FR(i) (RD_FRAGS[LB.fbase + (i)])

static inline void lb_begin_line(void) {                    /* first fragment of a new line: apply the pending margin */
    if (LB.nf == 0 && !LB.active) {
        LB.cur->y += LB.cur->pend; LB.cur->pend = 0;
        LB.line_y = LB.cur->y;
        LB.active = 1;
        LB.pen = LB.first_line ? LB.indent : 0;
    }
}

/* Merges neighbouring words of one run into a single text item and drops dead items, but only inside [from, to);
 * everything after `to` is slid down to close the gap. Returns how many items vanished. */
static inline int lb_compact(int from, int to) {
    int w = from;
    for (int r = from; r < to; r++) {
        rd_item_t *it = &RD_ITEMS[r];
        if (it->kind == DL_NONE) continue;
        if (w > from && it->kind == DL_TEXT) {
            rd_item_t *pv = &RD_ITEMS[w - 1];
            if (pv->kind == DL_TEXT && pv->y == it->y && pv->cell == it->cell && pv->flags == it->flags && pv->color == it->color &&
                pv->color2 == it->color2 && pv->ref == it->ref && pv->h == it->h && !(it->flags & TF_PRE) &&
                it->x == pv->x + pv->w + pv->cell && it->off >= pv->off + pv->len) {
                int ok = 1;                                       /* only whitespace may sit between the two words in the pool */
                for (u32 q = pv->off + pv->len; q < it->off; q++) if (!dom_is_ws(RD_POOL[q])) { ok = 0; break; }
                if (ok && (u32)(it->off + it->len - pv->off) < 65535) {
                    pv->len = (u16)(it->off + it->len - pv->off);
                    pv->w = (short)(pv->w + pv->cell + it->w);
                    continue;
                }
            }
        }
        if (w != r) RD_ITEMS[w] = *it;
        w++;
    }
    int removed = to - w;
    if (removed > 0) {
        for (int r = to; r < (int)L.n; r++) RD_ITEMS[r - removed] = RD_ITEMS[r];
        L.n -= (u32)removed;
    }
    return removed;
}

/* Finalizes fragments [0,n) of this builder: vertical metrics, text-align, translation into place, merging.
 * `total` is how many fragments exist (the ones in [n,total) are still waiting; their item indices are fixed up).
 * cur->y advances past the line. Returns the number of display-list items removed by merging. */
static inline int lb_finish(int n, int total) {
    if (!LB.active) lb_begin_line();
    int asc = LB.strut_asc, desc = LB.strut_h - LB.strut_asc;
    for (int i = 0; i < n; i++) {
        rd_frag_t *f = &FR(i);
        if (f->valign == VA_TOP || f->valign == VA_BOTTOM || f->valign == VA_MIDDLE) continue;
        if (f->asc > asc) asc = f->asc;
        if (f->h - f->asc > desc) desc = f->h - f->asc;
    }
    int lh = asc + desc;
    for (int i = 0; i < n; i++) {                              /* top/middle/bottom-aligned boxes may be taller than the rest */
        rd_frag_t *f = &FR(i);
        if ((f->valign == VA_TOP || f->valign == VA_BOTTOM || f->valign == VA_MIDDLE) && f->h > lh) { desc += f->h - lh; lh = f->h; }
    }
    int minx = 0, maxx = 0;
    if (n > 0) { minx = FR(0).x; maxx = FR(n - 1).x + FR(n - 1).w; }
    int shift = 0;
    if (LB.text_align == TA_CENTER) shift = (LB.w - (maxx - minx)) / 2 - minx;
    else if (LB.text_align == TA_RIGHT) shift = LB.w - maxx;
    if (shift < 0) shift = 0;
    for (int i = 0; i < n; i++) {
        rd_frag_t *f = &FR(i);
        int dy;
        switch (f->valign) {
            case VA_TOP: dy = 0; break;
            case VA_BOTTOM: dy = lh - f->h; break;
            case VA_MIDDLE: dy = (lh - f->h) / 2; break;
            default: dy = asc - f->asc; break;
        }
        dl_translate(f->item0, f->item1, LB.x0 + f->x + shift, LB.line_y + dy);
    }
    int removed = 0;
    if (n > 0) {
        removed = lb_compact(FR(0).item0, FR(n - 1).item1);
        if (removed) for (int i = n; i < total; i++) { FR(i).item0 -= removed; FR(i).item1 -= removed; }
    }
    if (L.mark_item >= 0 && L.mark_item < (int)L.n) {           /* a list marker was waiting for its first line */
        RD_ITEMS[L.mark_item].y = LB.line_y + asc - (L.mark_cell * 9) / 11;
        L.mark_item = -1;
    }
    LB.cur->y = LB.line_y + lh;
    LB.first_line = 0;
    LB.active = 0; LB.space = 0; LB.pen = 0;
    return removed;
}

/* finish the whole line */
static inline void lb_flush(void) { if (LB.nf > 0) { int n = LB.nf; LB.nf = 0; lb_finish(n, n); } }

/* The new fragment doesn't fit on this line. If it was preceded by whitespace the break goes right before it;
 * otherwise (a glued-on piece like the "," after a link) the tail since the last break opportunity moves down with it.
 * Returns the number of items removed from the list (the caller's not-yet-placed fragment must shift by that much). */
static inline int lb_wrap(int new_has_gap) {
    int total = LB.nf, k = -1;
    if (!new_has_gap) for (int i = total - 1; i >= 1; i--) if (FR(i).brk) { k = i; break; }
    if (k < 0 || total - k > 24) { LB.nf = 0; return lb_finish(total, total); }
    rd_frag_t save[24];
    int tail = total - k;
    int removed = lb_finish(k, total);
    for (int i = 0; i < tail; i++) save[i] = FR(k + i);
    LB.nf = 0;
    lb_begin_line();
    int dx = save[0].x - LB.pen;
    for (int i = 0; i < tail; i++) { FR(i) = save[i]; FR(i).x = (short)(FR(i).x - dx); if (i == 0) FR(i).brk = 0; }
    LB.nf = tail;
    LB.pen = FR(tail - 1).x + FR(tail - 1).w;
    return removed;
}

/* Places a prepared fragment: its items are already at the end of the display list (from `item0`), at
 * fragment-local coordinates. gap = the space that precedes it if it follows something on this line. */
static inline void lb_add(int item0, int w, int h, int asc, int valign, int gap, int may_break, int kind) {
    if (LB.fbase + LB.nf >= RD_FRAG_MAX - 1) lb_flush();
    lb_begin_line();
    int g = (LB.nf > 0) ? gap : 0;
    if (LB.nf > 0 && may_break && LB.pen + g + w > LB.w) {
        int had_gap = g > 0;
        item0 -= lb_wrap(had_gap);
        lb_begin_line();
        g = (LB.nf > 0 && had_gap) ? gap : 0;
    }
    rd_frag_t *f = &FR(LB.nf);
    f->item0 = item0; f->item1 = (int)L.n; f->x = (short)(LB.pen + g); f->w = (short)w; f->h = (short)h; f->asc = (short)asc;
    f->valign = (u8)valign; f->brk = (u8)(g > 0 && may_break); f->kind = (u8)kind; f->pad = 0;
    LB.pen += g + w;
    LB.nf++;
    LB.space = 0;
}

/* an empty line (a <br> with nothing before it, or a blank line in <pre>) */
static inline void lb_empty_line(void) {
    lb_begin_line();
    lb_finish(0, 0);
}
static inline void lb_br(void) {
    if (LB.nf > 0) { int n = LB.nf; LB.nf = 0; lb_finish(n, n); }
    else lb_empty_line();
}

/* ============================================================
 * helpers
 * ============================================================ */
static inline int rs(css_len_t l, int base) { return css_len_is_auto(l) ? 0 : css_len_resolve(l, base); }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

/* <details> state: which ones the user flipped from what the markup says (a short list -- nobody opens 32 at once) */
#define RD_TOGGLE_MAX 32
static u32 rd_toggled[RD_TOGGLE_MAX];
static int rd_ntoggled;
static inline int rd_is_toggled(u32 node) { for (int i = 0; i < rd_ntoggled; i++) if (rd_toggled[i] == node) return 1; return 0; }
static inline void rd_toggle_flip(u32 node) {
    for (int i = 0; i < rd_ntoggled; i++) if (rd_toggled[i] == node) { rd_toggled[i] = rd_toggled[--rd_ntoggled]; return; }
    if (rd_ntoggled < RD_TOGGLE_MAX) rd_toggled[rd_ntoggled++] = node;
}
static inline int rd_node_open(u32 node) {
    int o = dom_has_attr(node, AT_OPEN);
    if (rd_is_toggled(node)) o = !o;
    return o;
}

static inline int lay_is_block_display(int d) {
    return d == DISP_BLOCK || d == DISP_LIST_ITEM || d == DISP_TABLE || d == DISP_FLEX || d == DISP_GRID ||
           d == DISP_TABLE_ROW || d == DISP_TABLE_CELL || d == DISP_TABLE_GROUP || d == DISP_TABLE_CAPTION;
}

static void lay_children(u32 parent, const css_style_t *pst, lbox_t *cur);
static void lay_block(u32 node, const css_style_t *st, lbox_t *pc);
static void lay_table(u32 node, const css_style_t *st, lbox_t *pc);
static void lay_flex_content(u32 node, const css_style_t *st, lbox_t *in);
static void lay_grid_content(u32 node, const css_style_t *st, lbox_t *in);
static void lay_content(u32 node, const css_style_t *st, lbox_t *in);
static void lay_intrinsic(u32 node, const css_style_t *st, int *mn, int *mx);

/* ============================================================
 * replaced elements and form controls: their sizes
 * ============================================================ */
enum { RK_NONE = 0, RK_IMG, RK_CTL, RK_EMPTY };

static inline int lay_input_kind(u32 node) {
    u32 tl; const u8 *t = dom_attr(node, AT_TYPE, &tl);
    if (!t || tl == 0) return CTL_TEXT;
    if (css_kw(t, tl, "password")) return CTL_PASSWORD;
    if (css_kw(t, tl, "submit") || css_kw(t, tl, "button") || css_kw(t, tl, "reset") || css_kw(t, tl, "image") || css_kw(t, tl, "file")) return CTL_BUTTON;
    if (css_kw(t, tl, "checkbox")) return CTL_CHECK;
    if (css_kw(t, tl, "radio")) return CTL_RADIO;
    return CTL_TEXT;
}
static inline int lay_attr_int(u32 node, u32 id, int dflt) {
    u32 l; const u8 *v = dom_attr(node, id, &l);
    int x;
    if (v && css_num(v, l, &x) && x > 0) return x / 100;
    return dflt;
}
/* text of an element's descendants, measured in cells (used for <option> labels) */
static inline u32 lay_text_cells(u32 node) {
    u32 total = 0;
    for (u32 c = RD_NODES[node].first; c; c = RD_NODES[c].next) {
        if (RD_NODES[c].kind == RDK_TEXT) {
            const u8 *t = RD_POOL + RD_NODES[c].a; u32 n = RD_NODES[c].b; int sp = 1;
            for (u32 i = 0; i < n; i++) {
                if (dom_is_ws(t[i])) { if (!sp) total++; sp = 1; } else { sp = 0; if (t[i] >= 0xE0) i += 2; total++; }
            }
        } else total += lay_text_cells(c);
    }
    return total;
}
static inline u32 lay_select_cells(u32 node) {
    u32 best = 0;
    for (u32 c = RD_NODES[node].first; c; c = RD_NODES[c].next) {
        if (RD_NODES[c].tag == TG_OPTION) { u32 k = lay_text_cells(c); if (k > best) best = k; }
        else if (RD_NODES[c].tag == TG_OPTGROUP) { u32 k = lay_select_cells(c); if (k > best) best = k; }
    }
    return best;
}

/* Fills the CONTENT-box size of a replaced element. Returns RK_NONE for ordinary boxes. */
static inline int lay_replaced_size(u32 node, const css_style_t *st, int avail, int *cw, int *ch, int *ctl) {
    int tag = RD_NODES[node].tag, c = st->cell;
    int w = -1, h = -1, rk = RK_NONE;
    *ctl = 0;
    switch (tag) {
        case TG_IMG: {
            rk = RK_IMG;
            if (!css_len_is_auto(st->width)) w = css_len_resolve(st->width, avail);
            if (!css_len_is_auto(st->height) && !st->height.pct) h = st->height.px;
            if (w < 0 && h < 0) {
                u32 al; const u8 *a = dom_attr(node, AT_ALT, &al);
                if (a && al) { w = (int)txt_cells(a, al) * c + 6; h = c + 6; } else { w = h = c + 5; }
            } else if (h < 0) h = imin(w * 5 / 8, 240);
            else if (w < 0) w = imin(h * 4 / 3, 600);
            return (*cw = w, *ch = h, rk);
        }
        case TG_INPUT: {
            int k = lay_input_kind(node); rk = RK_CTL; *ctl = k;
            u32 vl; const u8 *v = dom_attr(node, AT_VALUE, &vl);
            switch (k) {
                case CTL_CHECK: case CTL_RADIO: w = h = c + 1; break;
                case CTL_BUTTON: {
                    u32 cells = v ? txt_cells(v, vl) : 6;
                    u32 tl; const u8 *t = dom_attr(node, AT_TYPE, &tl);
                    if (!v && t && css_kw(t, tl, "reset")) cells = 5;
                    if (!v && t && css_kw(t, tl, "file")) cells = 14;
                    w = (int)cells * c; h = c + 1; break;
                }
                default: w = lay_attr_int(node, AT_SIZE, 20) * c * 7 / 8; h = c + 1; break;
            }
            break;
        }
        case TG_SELECT: rk = RK_CTL; *ctl = CTL_SELECT; w = (int)(lay_select_cells(node) + 3) * c; if (w < 5 * c) w = 5 * c; h = c + 1; break;
        case TG_TEXTAREA: {
            rk = RK_CTL; *ctl = CTL_TEXTAREA;
            w = lay_attr_int(node, AT_COLS, 20) * c * 7 / 8; h = lay_attr_int(node, AT_ROWS, 2) * (c + 2) + 2; break;
        }
        case TG_SVG: case TG_CANVAS: case TG_VIDEO: case TG_AUDIO: case TG_IFRAME: case TG_OBJECT: case TG_EMBED: case TG_MATH:
        case TG_PROGRESS: case TG_METER:
            rk = RK_EMPTY; w = 0; h = 0;
            if (tag == TG_PROGRESS || tag == TG_METER) { w = 8 * c; h = c; }
            break;
        default: return RK_NONE;
    }
    if (!css_len_is_auto(st->width)) { w = css_len_resolve(st->width, avail); }
    if (!css_len_is_auto(st->height) && !st->height.pct) h = st->height.px;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    *cw = w; *ch = h;
    return rk;
}

/* paints a replaced element's own content at absolute (cx,cy) */
static inline void lay_emit_replaced(u32 node, const css_style_t *st, int rk, int ctl, int cx, int cy, int cw, int ch) {
    if (rk == RK_IMG) {
        rd_item_t *it = dl_new();
        u32 al; const u8 *a = dom_attr(node, AT_ALT, &al);
        it->kind = DL_IMG; it->x = (short)cx; it->y = cy; it->w = (short)cw; it->h = (short)ch; it->color = st->color; it->cell = st->cell;
        it->off = a ? (u32)(a - RD_POOL) : 0; it->len = (u16)(a ? (al > 255 ? 255 : al) : 0); it->ref = L.link;
        if (st->visibility) it->kind = DL_NONE;
    } else if (rk == RK_CTL) {
        rd_item_t *it = dl_new();
        it->kind = st->visibility ? DL_NONE : DL_CTL; it->aux = (u8)ctl; it->x = (short)cx; it->y = cy; it->w = (short)cw; it->h = (short)ch;
        it->color = st->color; it->cell = st->cell; it->ref = node;
    }
}

/* ============================================================
 * text
 * ============================================================ */
typedef struct { int cell, lh, asc, valign; u8 flags; u32 color, bg; int visible; } txt_ctx_t;

static inline void lay_make_ctx(const css_style_t *st, txt_ctx_t *x) {
    x->cell = st->cell; x->lh = st_line_height(st); x->asc = st_asc(st, x->lh);
    x->valign = st->valign == VA_SUB || st->valign == VA_SUPER ? VA_BASELINE : st->valign;
    if (st->valign == VA_SUPER) { x->asc += st->cell * 3 / 8; x->lh += st->cell * 3 / 8; }
    if (st->valign == VA_SUB) { x->asc -= st->cell / 4; }
    u8 f = 0;
    if (st->flags & SF_BOLD) f |= TF_BOLD;
    if (st->flags & SF_ITALIC) f |= TF_ITALIC;
    if (st->flags & SF_UNDER) f |= TF_UNDER;
    if (st->flags & SF_STRIKE) f |= TF_STRIKE;
    if (st->text_transform == TT_UPPER) f |= TF_UPPER; else if (st->text_transform == TT_LOWER) f |= TF_LOWER; else if (st->text_transform == TT_CAP) f |= TF_CAP;
    if (st->white_space == WS_PRE || st->white_space == WS_PRE_WRAP) f |= TF_PRE;
    x->flags = f; x->color = st->color;
    x->bg = (st->display == DISP_INLINE) ? st->bg : CSS_NOCOLOR;
    x->visible = !(st->visibility || st->hidden_text);
}

/* one chunk of text as a fragment */
static inline void lay_put_text(const txt_ctx_t *x, u32 off, u32 nb, u32 cells, int gap_cells, int may_break) {
    int w = (int)cells * x->cell;
    int i0 = (int)L.n;
    rd_item_t *it = dl_new();
    it->kind = x->visible ? DL_TEXT : DL_NONE; it->w = (short)w; it->h = (short)x->lh; it->len = (u16)nb; it->off = off;
    it->cell = (u8)x->cell; it->flags = x->flags; it->color = x->color; it->color2 = x->bg; it->ref = L.link;
    lb_add(i0, w, x->lh, x->asc, x->valign, gap_cells * x->cell, may_break, 0);
}

/* a word (or any unbreakable chunk); breaks it by characters if it is wider than the whole line */
static inline void lay_word(const txt_ctx_t *x, u32 off, u32 nb, u32 cells, int gap_cells, int may_break) {
    int w = (int)cells * x->cell;
    if (!(w > LB.w && may_break && LB.w >= x->cell)) { lay_put_text(x, off, nb, cells, gap_cells, may_break); return; }
    u32 pos = off, rem_b = nb, rem_c = cells; int first = 1;
    while (rem_c > 0 && !L.full) {
        int used = LB.nf > 0 ? LB.pen + (first ? gap_cells * x->cell : 0) : (LB.first_line ? LB.indent : 0);
        int avail = (LB.w - used) / x->cell;
        if (avail < 1) { if (LB.nf > 0) { lb_flush(); continue; } avail = 1; }
        u32 take = rem_c < (u32)avail ? rem_c : (u32)avail, nbytes = 0;
        for (u32 k = 0; k < take; k++) nbytes += RD_POOL[pos + nbytes] >= 0xE0 ? 3 : 1;
        lay_put_text(x, pos, nbytes, take, first ? gap_cells : 0, first ? may_break : 0);
        pos += nbytes; rem_b -= nbytes; rem_c -= take; first = 0;
        if (rem_c > 0) lb_flush();
    }
    (void)rem_b;
}

static void lay_text(u32 node, const css_style_t *pst) {
    rd_node_t *tn = &RD_NODES[node];
    const u8 *t = RD_POOL + tn->a; u32 len = tn->b, i = 0;
    txt_ctx_t x; lay_make_ctx(pst, &x);
    int ws = pst->white_space;
    int collapse = ws == WS_NORMAL || ws == WS_NOWRAP || ws == WS_PRE_LINE;
    int may_break = ws != WS_NOWRAP;
    if (collapse) {
        while (i < len && !L.full) {
            u8 c = t[i];
            if (dom_is_ws(c)) {
                if (c == '\n' && ws == WS_PRE_LINE) { lb_br(); }
                else LB.space = 1;
                i++; continue;
            }
            u32 j = i, cells = 0;
            while (j < len && !dom_is_ws(t[j])) { j += t[j] >= 0xE0 ? 3 : 1; cells++; }
            lay_word(&x, tn->a + i, j - i, cells, (LB.space && LB.nf > 0) ? 1 : 0, may_break);
            i = j;
        }
        return;
    }
    /* pre / pre-wrap: whitespace is literal, newlines end lines */
    while (i < len && !L.full) {
        u32 j = i; while (j < len && t[j] != '\n') j++;
        if (j > i) {
            if (ws == WS_PRE) {
                lay_put_text(&x, tn->a + i, j - i, txt_cells(t + i, j - i), 0, 0);
            } else {
                u32 k = i;
                while (k < j) {
                    u32 sp = 0; while (k < j && t[k] == ' ') { sp++; k++; }
                    u32 e = k, cells = 0;
                    while (e < j && t[e] != ' ') { e += t[e] >= 0xE0 ? 3 : 1; cells++; }
                    if (cells == 0) { LB.pen += (LB.nf == 0 ? 0 : 0); break; }
                    if (LB.nf == 0 && sp > 0) { lb_begin_line(); LB.pen += (int)sp * x.cell; sp = 0; }
                    lay_word(&x, tn->a + k, e - k, cells, (int)sp, 1);
                    k = e;
                }
            }
        }
        if (j < len) {
            if (j == i && LB.nf == 0) lb_empty_line(); else lb_flush();
            i = j + 1;
        } else i = j;
    }
}

/* ============================================================
 * list markers
 * ============================================================ */
static inline void lay_roman(u32 v, int upper, char *out, u32 *len) {
    static const struct { int v; const char *s; } t[] = { {1000,"m"},{900,"cm"},{500,"d"},{400,"cd"},{100,"c"},{90,"xc"},{50,"l"},{40,"xl"},{10,"x"},{9,"ix"},{5,"v"},{4,"iv"},{1,"i"} };
    u32 o = 0;
    if (v == 0 || v > 3999) { out[o++] = '0' + (v % 10); *len = o; return; }
    for (unsigned k = 0; k < sizeof(t) / sizeof(t[0]); k++)
        while ((int)v >= t[k].v) { for (const char *p = t[k].s; *p; p++) out[o++] = upper ? (char)(*p - 32) : *p; v -= (u32)t[k].v; }
    *len = o;
}

static inline void lay_list_marker(u32 node, const css_style_t *st, int cx) {
    int ls = st->list_style, c = st->cell;
    if (ls == LS_NONE || L.full) return;
    rd_item_t *it = dl_new();
    L.mark_item = (int)L.n - 1; L.mark_cell = c;
    it->color = st->color; it->cell = (u8)c; it->ref = 0; it->h = (short)c;
    if (st->visibility) { it->kind = DL_NONE; return; }
    if (ls == LS_DISC || ls == LS_CIRCLE || ls == LS_SQUARE) {
        it->kind = DL_BULLET; it->aux = (u8)(ls == LS_DISC ? BUL_DISC : ls == LS_CIRCLE ? BUL_CIRCLE : BUL_SQUARE);
        it->w = (short)c; it->x = (short)(cx - c - 2);
        return;
    }
    u32 num = 1; u32 p = RD_NODES[node].parent;
    if (p) {
        u32 sl; const u8 *sv = dom_attr(p, AT_START, &sl); int x;
        if (sv && css_num(sv, sl, &x)) num = (u32)(x / 100);
        for (u32 s = RD_NODES[p].first; s && s != node; s = RD_NODES[s].next) {
            if (RD_NODES[s].kind != RDK_ELEM || RD_NODES[s].tag != TG_LI) continue;
            u32 vl; const u8 *vv = dom_attr(s, AT_VALUE, &vl);
            if (vv && css_num(vv, vl, &x)) num = (u32)(x / 100);
            num++;
        }
    }
    u32 vl; const u8 *vv = dom_attr(node, AT_VALUE, &vl); int xv;
    if (vv && css_num(vv, vl, &xv)) num = (u32)(xv / 100);
    char buf[16]; u32 bl = 0;
    if (ls == LS_LOWER_ALPHA || ls == LS_UPPER_ALPHA) { buf[bl++] = (char)((ls == LS_UPPER_ALPHA ? 'A' : 'a') + (int)((num + 25) % 26)); }
    else if (ls == LS_LOWER_ROMAN || ls == LS_UPPER_ROMAN) lay_roman(num, ls == LS_UPPER_ROMAN, buf, &bl);
    else { char d[12]; int nd = 0; u32 v = num; do { d[nd++] = (char)('0' + v % 10); v /= 10; } while (v && nd < 10); while (nd) buf[bl++] = d[--nd]; }
    buf[bl++] = '.';
    it->kind = DL_TEXT; it->color2 = CSS_NOCOLOR; it->off = dom_pool_len; it->len = (u16)bl; it->w = (short)((int)bl * c);
    for (u32 k = 0; k < bl; k++) dom_pool_putc((u8)buf[k]);
    it->x = (short)(cx - (int)bl * c - c / 2);
    it->flags = 0;
}

/* ============================================================
 * intrinsic (min-content / max-content) widths
 * ============================================================ */
typedef struct { int mn, mx, line, run; } intr_t;
static inline void intr_flush(intr_t *a) {
    if (a->line > a->mx) a->mx = a->line;
    if (a->run > a->mn) a->mn = a->run;
    a->line = 0; a->run = 0;
}
static inline void intr_text(u32 node, const css_style_t *pst, intr_t *a) {
    rd_node_t *tn = &RD_NODES[node];
    const u8 *t = RD_POOL + tn->a; u32 len = tn->b, i = 0;
    int c = pst->cell, ws = pst->white_space;
    if (ws == WS_PRE || ws == WS_PRE_WRAP) {
        while (i <= len) {
            u32 j = i; while (j < len && t[j] != '\n') j++;
            int w = (int)txt_cells(t + i, j - i) * c;
            a->line += w; a->run += (ws == WS_PRE ? w : 0);
            if (j < len) intr_flush(a);
            i = j + 1;
        }
        return;
    }
    int sp = 0;
    while (i < len) {
        if (dom_is_ws(t[i])) { sp = 1; i++; continue; }
        u32 j = i, cells = 0; while (j < len && !dom_is_ws(t[j])) { j += t[j] >= 0xE0 ? 3 : 1; cells++; }
        int w = (int)cells * c;
        if (sp && a->line > 0) { a->line += c; if (ws != WS_NOWRAP) { if (a->run > a->mn) a->mn = a->run; a->run = 0; } else a->run += c; }
        a->line += w; a->run += w;
        sp = 0; i = j;
    }
    if (sp && a->line > 0) { a->line += c; if (ws != WS_NOWRAP) { if (a->run > a->mn) a->mn = a->run; a->run = 0; } }
}

static inline void lay_box_extras(const css_style_t *st, int base, int *left, int *right) {
    int ml = rs(st->margin[3], base), mr = rs(st->margin[1], base);
    *left = ml + rs(st->padding[3], base) + st->bw[3];
    *right = mr + rs(st->padding[1], base) + st->bw[1];
}

static void intr_node(u32 node, const css_style_t *pst, intr_t *a, int base);

static inline void intr_children(u32 parent, const css_style_t *pst, intr_t *a, int base) {
    if (L.depth > RD_MAX_DEPTH) return;
    L.depth++;
    int only_summary = (RD_NODES[parent].tag == TG_DETAILS && !rd_node_open(parent));
    for (u32 c = RD_NODES[parent].first; c; c = RD_NODES[c].next) {
        if (only_summary && RD_NODES[c].tag != TG_SUMMARY) continue;
        intr_node(c, pst, a, base);
    }
    L.depth--;
}

static void intr_node(u32 node, const css_style_t *pst, intr_t *a, int base) {
    rd_node_t *n = &RD_NODES[node];
    if (n->kind == RDK_TEXT) { intr_text(node, pst, a); return; }
    if (n->kind != RDK_ELEM) return;
    css_style_t cs; css_compute(node, pst, &cs);
    if (cs.display == DISP_NONE) return;
    if (n->tag == TG_BR) { intr_flush(a); return; }
    int l, r; lay_box_extras(&cs, base, &l, &r);
    int cw, ch, ctl, rk = lay_replaced_size(node, &cs, base, &cw, &ch, &ctl);
    int hor = l + r + cs.padding[1].px * 0;
    if (lay_is_block_display(cs.display)) {
        intr_flush(a);
        int mn, mx;
        if (rk) { mn = mx = cw; }
        else if (!css_len_is_auto(cs.width) && !cs.width.pct) { mn = mx = imax(0, cs.width.px - (cs.box_sizing_border ? (l + r) : 0)); }
        else lay_intrinsic(node, &cs, &mn, &mx);
        if (mx + l + r > a->mx) a->mx = mx + l + r;
        if (mn + l + r > a->mn) a->mn = mn + l + r;
        return;
    }
    if (rk || cs.display == DISP_INLINE_BLOCK || cs.display == DISP_INLINE_FLEX || cs.display == DISP_INLINE_TABLE) {
        int mn, mx;
        if (rk) { mn = mx = cw; }
        else if (!css_len_is_auto(cs.width) && !cs.width.pct) { mn = mx = imax(0, cs.width.px - (cs.box_sizing_border ? (l + r) : 0)); }
        else lay_intrinsic(node, &cs, &mn, &mx);
        a->line += mx + hor; a->run += mn + hor;
        if (a->run > a->mn) a->mn = a->run;
        a->run = 0;
        return;
    }
    a->line += l; a->run += l;
    intr_children(node, &cs, a, base);
    a->line += r; a->run += r;
}

/* content-box min/max width of a container's contents */
static void lay_intrinsic(u32 node, const css_style_t *st, int *mn, int *mx) {
    rd_node_t *n = &RD_NODES[node];
    if (n->flags & 1) { *mn = (int)(n->aux & 0xFFFF); *mx = (int)(n->aux >> 16); return; }
    intr_t a = { 0, 0, 0, 0 };
    switch (st->display) {
        case DISP_TABLE: case DISP_INLINE_TABLE: {
            int sx = st->border_collapse ? 0 : st->spacing_x;
            for (u32 g = n->first; g; g = RD_NODES[g].next) {
                if (RD_NODES[g].kind != RDK_ELEM) continue;
                css_style_t gs; css_compute(g, st, &gs);
                if (gs.display == DISP_NONE) continue;
                u32 rows[2]; int nr = 0; u32 gnode = g;
                (void)rows; (void)nr; (void)gnode;
                u32 iter_first = (gs.display == DISP_TABLE_GROUP) ? RD_NODES[g].first : g;
                for (u32 r = iter_first; r; r = (gs.display == DISP_TABLE_GROUP) ? RD_NODES[r].next : 0) {
                    css_style_t rsty; const css_style_t *rst;
                    if (gs.display == DISP_TABLE_GROUP) { if (RD_NODES[r].kind != RDK_ELEM) continue; css_compute(r, &gs, &rsty); rst = &rsty; }
                    else rst = &gs;
                    if (rst->display != DISP_TABLE_ROW) continue;
                    int rmn = sx, rmx = sx;
                    for (u32 cell = RD_NODES[r].first; cell; cell = RD_NODES[cell].next) {
                        if (RD_NODES[cell].kind != RDK_ELEM) continue;
                        css_style_t cs; css_compute(cell, rst, &cs);
                        if (cs.display == DISP_NONE) continue;
                        int l, rr, cmn, cmx; lay_box_extras(&cs, 0, &l, &rr);
                        lay_intrinsic(cell, &cs, &cmn, &cmx);
                        if (!css_len_is_auto(cs.width) && !cs.width.pct && cs.width.px > cmx) cmx = cs.width.px;
                        int cols = lay_attr_int(cell, AT_COLSPAN, 1);
                        rmn += cmn + l + rr + sx; rmx += cmx + l + rr + sx; (void)cols;
                    }
                    if (rmn > a.mn) a.mn = rmn;
                    if (rmx > a.mx) a.mx = rmx;
                }
            }
            break;
        }
        case DISP_FLEX: case DISP_INLINE_FLEX: {
            int col = st->flex_dir == FD_COLUMN, sum_mn = 0, sum_mx = 0, gaps = 0;
            for (u32 c = n->first; c; c = RD_NODES[c].next) {
                intr_t b = { 0, 0, 0, 0 };
                if (RD_NODES[c].kind == RDK_TEXT) { intr_text(c, st, &b); intr_flush(&b); }
                else if (RD_NODES[c].kind == RDK_ELEM) {
                    css_style_t cs; css_compute(c, st, &cs);
                    if (cs.display == DISP_NONE) continue;
                    int l, r, cmn, cmx, cw, ch, ctl, rk = lay_replaced_size(c, &cs, 0, &cw, &ch, &ctl);
                    lay_box_extras(&cs, 0, &l, &r);
                    if (rk) cmn = cmx = cw; else if (!css_len_is_auto(cs.width) && !cs.width.pct) cmn = cmx = cs.width.px; else lay_intrinsic(c, &cs, &cmn, &cmx);
                    b.mn = cmn + l + r; b.mx = cmx + l + r;
                } else continue;
                if (col) { if (b.mn > a.mn) a.mn = b.mn; if (b.mx > a.mx) a.mx = b.mx; }
                else { sum_mn += b.mn; sum_mx += b.mx; if (b.mn > a.mn) a.mn = b.mn; gaps += st->gap_col; }
            }
            if (!col) { a.mx = sum_mx + gaps; if (!st->flex_wrap) a.mn = sum_mn + gaps; }
            break;
        }
        default:
            intr_children(node, st, &a, 0);
            intr_flush(&a);
            break;
    }
    intr_flush(&a);
    *mn = imin(a.mn, 4000); *mx = imin(a.mx, 4000);
    if (*mn > *mx) *mx = *mn;
    n->aux = (u32)(*mn & 0xFFFF) | ((u32)(*mx & 0xFFFF) << 16);
    n->flags |= 1;
}

/* ============================================================
 * inline-level boxes
 * ============================================================ */
static void lay_inline_elem(u32 node, const css_style_t *st, const css_style_t *pst);

static inline int st_height_decl(const css_style_t *st) { return (!css_len_is_auto(st->height) && st->height.pct == 0) ? st->height.px : -1; }

/* applies min-/max-width (content box) to a width */
static inline int lay_clamp_w(const css_style_t *st, int w, int base, int hor) {
    if (!css_len_is_auto(st->maxw)) { int m = css_len_resolve(st->maxw, base); if (st->box_sizing_border) m -= hor; if (w > m) w = m; }
    if (!css_len_is_auto(st->minw)) { int m = css_len_resolve(st->minw, base); if (st->box_sizing_border) m -= hor; if (w < m) w = m; }
    return w < 0 ? 0 : w;
}

/* a zero-height-ish fragment that only moves the pen (inline padding / margin / border), optionally painted with the element's background */
static inline void lay_spacer(const css_style_t *st, int w, int with_gap, int start) {
    if (w <= 0) return;
    txt_ctx_t x; lay_make_ctx(st, &x);
    int i0 = (int)L.n;
    if (x.bg != CSS_NOCOLOR && x.visible) dl_rect(0, 0, w, x.lh, x.bg);
    lb_add(i0, w, x.lh, x.asc, x.valign, with_gap ? st->cell : 0, start, 2);
}

/* img / input / button / select / inline-block / ...: a box that sits on the line as one unbreakable thing */
static void lay_atomic(u32 node, const css_style_t *st, const css_style_t *pst) {
    int avail = LB.w, tag = RD_NODES[node].tag;
    int mt = rs(st->margin[0], avail), mr = rs(st->margin[1], avail), mb = rs(st->margin[2], avail), ml = rs(st->margin[3], avail);
    int pt = rs(st->padding[0], avail), pr = rs(st->padding[1], avail), pb = rs(st->padding[2], avail), pl = rs(st->padding[3], avail);
    int bt = st->bw[0], br = st->bw[1], bb = st->bw[2], bl = st->bw[3];
    int hor = pl + pr + bl + br, ver = pt + pb + bt + bb;
    int cw, ch, ctl, rk = lay_replaced_size(node, st, avail, &cw, &ch, &ctl);
    int i0 = (int)L.n;
    u32 saved_link = L.link;
    if (tag == TG_A && dom_has_attr(node, AT_HREF)) L.link = node;
    dl_box_t bx = dl_box_reserve(st);
    int hbb, extra = 0;
    if (st->display == DISP_INLINE_TABLE) {                       /* a table that sits on the line: lay it out at the origin, then place it */
        lbox_t tp; tp.x = 0; tp.w = avail; tp.y = 0; tp.pend = 0;
        css_style_t t2 = *st; t2.display = DISP_TABLE;
        lay_table(node, &t2, &tp);
        int th = tp.y + tp.pend;
        lb_add(i0, L.last_w, th, th, VA_BASELINE, LB.space ? pst->cell : 0, 1, 1);
        L.link = saved_link;
        return;
    }
    if (rk) {
        if (rk == RK_IMG && avail > 0 && cw + hor + ml + mr > avail && cw > 0) {          /* too wide for the line: scale to fit */
            int nw = imax(1, avail - hor - ml - mr);
            ch = imax(1, ch * nw / cw); cw = nw;
        }
        cw = lay_clamp_w(st, cw, avail, hor);
        hbb = ch + ver;
        lay_emit_replaced(node, st, rk, ctl, ml + bl + pl, mt + bt + pt, cw, ch);
    } else {
        int room = imax(0, avail - ml - mr - hor);
        if (!css_len_is_auto(st->width)) { cw = css_len_resolve(st->width, avail); if (st->box_sizing_border) cw -= hor; }
        else { int mn, mx; lay_intrinsic(node, st, &mn, &mx); cw = imax(mn, imin(mx, room)); }
        cw = lay_clamp_w(st, cw, avail, hor);
        lbox_t in; in.x = ml + bl + pl; in.w = cw; in.y = mt + bt + pt; in.pend = 0;
        lay_content(node, st, &in);
        int content_h = in.y + in.pend - (mt + bt + pt);
        int hd = st_height_decl(st);
        if (hd >= 0) content_h = st->box_sizing_border ? imax(0, hd - ver) : hd;
        if (!css_len_is_auto(st->minh) && st->minh.pct == 0 && content_h < st->minh.px) content_h = st->minh.px;
        ch = content_h; hbb = ch + ver;
        extra = pb + bb + (st->cell * 2) / 11;
        if (tag == TG_BUTTON) {
            rd_item_t *it = dl_new();
            it->kind = DL_CTL; it->aux = CTL_BUTTON; it->flags = 1; it->x = (short)ml; it->y = mt; it->w = (short)(cw + hor); it->h = (short)hbb; it->ref = node;
        }
    }
    dl_box_fill(bx, ml, mt, cw + hor, hbb, st, 0);
    L.link = saved_link;
    int va = (st->valign == VA_SUB || st->valign == VA_SUPER) ? VA_BASELINE : st->valign;
    lb_add(i0, ml + cw + hor + mr, mt + hbb + mb, mt + hbb - extra, va, LB.space ? pst->cell : 0, 1, 1);
}

static void lay_inline_elem(u32 node, const css_style_t *st, const css_style_t *pst) {
    int tag = RD_NODES[node].tag;
    if (tag == TG_BR) { lb_br(); return; }
    int d = st->display, cw, ch, ctl;
    int rk = lay_replaced_size(node, st, LB.w, &cw, &ch, &ctl);
    if (rk || tag == TG_BUTTON || d == DISP_INLINE_BLOCK || d == DISP_INLINE_FLEX || d == DISP_INLINE_TABLE) { lay_atomic(node, st, pst); return; }
    if (L.depth > RD_MAX_DEPTH) return;
    L.depth++;
    u32 saved_link = L.link;
    if (tag == TG_A && dom_has_attr(node, AT_HREF)) L.link = node;
    int ml = rs(st->margin[3], LB.w), pl = rs(st->padding[3], LB.w), mr = rs(st->margin[1], LB.w), pr = rs(st->padding[1], LB.w);
    lay_spacer(st, ml + pl + st->bw[3], LB.space, 1);
    int only_summary = (tag == TG_DETAILS && !rd_node_open(node));
    for (u32 c = RD_NODES[node].first; c && !L.full; c = RD_NODES[c].next) {
        if (only_summary && RD_NODES[c].tag != TG_SUMMARY) continue;
        rd_node_t *cn = &RD_NODES[c];
        if (cn->kind == RDK_TEXT) { lay_text(c, st); continue; }
        if (cn->kind != RDK_ELEM) continue;
        css_style_t cs; css_compute(c, st, &cs);
        if (cs.display == DISP_NONE) continue;
        if (lay_is_block_display(cs.display)) { lb_flush(); lay_block(c, &cs, LB.cur); }
        else lay_inline_elem(c, &cs, st);
    }
    lay_spacer(st, mr + pr + st->bw[1], 0, 0);
    L.link = saved_link;
    L.depth--;
}

/* ============================================================
 * block containers
 * ============================================================ */
static void lay_children(u32 parent, const css_style_t *pst, lbox_t *cur) {
    if (L.full || L.depth > RD_MAX_DEPTH) return;
    L.depth++;
    lb_t saved = LB;
    LB.x0 = cur->x; LB.w = cur->w; LB.cur = cur;
    LB.fbase = saved.fbase + saved.nf; LB.nf = 0; LB.active = 0; LB.space = 0; LB.pen = 0; LB.first_line = 1;
    LB.indent = pst->text_indent; LB.text_align = pst->text_align;
    int lh = st_line_height(pst);
    LB.strut_h = lh; LB.strut_asc = st_asc(pst, lh);
    int only_summary = (RD_NODES[parent].tag == TG_DETAILS && !rd_node_open(parent));
    for (u32 c = RD_NODES[parent].first; c && !L.full; c = RD_NODES[c].next) {
        rd_node_t *cn = &RD_NODES[c];
        if (only_summary && cn->tag != TG_SUMMARY) continue;
        if (cn->kind == RDK_TEXT) { lay_text(c, pst); continue; }
        if (cn->kind != RDK_ELEM) continue;
        css_style_t cs; css_compute(c, pst, &cs);
        if (cs.display == DISP_NONE) continue;
        if (lay_is_block_display(cs.display)) { lb_flush(); lay_block(c, &cs, cur); }
        else lay_inline_elem(c, &cs, pst);
    }
    lb_flush();
    LB = saved;
    L.depth--;
}

static void lay_block(u32 node, const css_style_t *st, lbox_t *pc) {
    if (L.full || L.depth > RD_MAX_DEPTH) return;
    if (st->display == DISP_TABLE) { lay_table(node, st, pc); return; }
    rd_node_t *n = &RD_NODES[node];
    int tag = n->tag, avail = pc->w;
    int mt = rs(st->margin[0], avail), mb = rs(st->margin[2], avail);
    int ml_auto = css_len_is_auto(st->margin[3]), mr_auto = css_len_is_auto(st->margin[1]);
    int ml = rs(st->margin[3], avail), mr = rs(st->margin[1], avail);
    int pt = rs(st->padding[0], avail), pr = rs(st->padding[1], avail), pb = rs(st->padding[2], avail), pl = rs(st->padding[3], avail);
    int bt = st->bw[0], br = st->bw[1], bb = st->bw[2], bl = st->bw[3];
    int hor = pl + pr + bl + br, ver = pt + pb + bt + bb;
    int cw = 0, ch = 0, ctl = 0, rk = lay_replaced_size(node, st, avail, &cw, &ch, &ctl);
    int w, declared = !css_len_is_auto(st->width) || rk;
    if (rk) w = cw;
    else if (!css_len_is_auto(st->width)) { w = css_len_resolve(st->width, avail); if (st->box_sizing_border) w -= hor; }
    else w = avail - ml - mr - hor;
    w = lay_clamp_w(st, w, avail, hor);
    if (w + hor + ml + mr > avail && declared) {
        int nw = imax(0, avail - ml - mr - hor);
        if (rk == RK_IMG && cw > 0) ch = imax(1, ch * nw / cw);
        w = nw;
    }
    if (w < 0) w = 0;
    int free_w = avail - (w + hor + ml + mr);
    if (free_w > 0) {
        if (ml_auto && mr_auto) { ml += free_w / 2; mr += free_w - free_w / 2; }
        else if (ml_auto) ml += free_w;
        else if (mr_auto) mr += free_w;
    }
    int hd = st_height_decl(st);
    if (hd >= 0 && st->box_sizing_border) hd = imax(0, hd - ver);
    int minh = (!css_len_is_auto(st->minh) && st->minh.pct == 0) ? st->minh.px : 0;
    int visible = st->bg != CSS_NOCOLOR || st_has_border(st);
    int has_h = hd >= 0 || minh > 0;
    int flush_top = visible || pt > 0 || bt > 0 || has_h || rk;
    lbox_t in;
    if (flush_top) { pc->y += mcombine(pc->pend, mt); pc->pend = 0; in.pend = 0; }
    else { in.pend = mcombine(pc->pend, mt); pc->pend = 0; }
    int top_y = pc->y;
    int bx_x = pc->x + ml;
    dl_box_t bx = dl_box_reserve(st);
    int cx = bx_x + bl + pl, content_top = top_y + bt + pt;
    in.x = cx; in.w = w; in.y = content_top;
    u32 saved_link = L.link;
    if (tag == TG_A && dom_has_attr(node, AT_HREF)) L.link = node;
    else if (tag == TG_SUMMARY && n->parent && RD_NODES[n->parent].tag == TG_DETAILS) L.link = node;   /* the whole summary line toggles */
    int marker_idx = -1;
    if (st->display == DISP_LIST_ITEM) { lay_list_marker(node, st, cx); marker_idx = L.mark_item; }
    else if (tag == TG_SUMMARY && n->parent && RD_NODES[n->parent].tag == TG_DETAILS) {
        rd_item_t *m = dl_new();
        m->kind = DL_BULLET; m->aux = (u8)(rd_node_open(n->parent) ? BUL_DOWN : BUL_RIGHT); m->w = (short)st->cell; m->h = (short)st->cell;
        m->x = (short)(cx - st->cell - 3); m->cell = st->cell; m->color = st->color; m->ref = node;
        L.mark_item = (int)L.n - 1; L.mark_cell = st->cell; marker_idx = L.mark_item;
    }
    if (rk) {
        lay_emit_replaced(node, st, rk, ctl, cx, content_top, w, ch);
        in.y = content_top + ch;
    } else if (!(st->overflow_hidden && hd == 0)) {
        lay_content(node, st, &in);
        if (tag == TG_BUTTON) {
            rd_item_t *it = dl_new();
            it->kind = DL_CTL; it->aux = CTL_BUTTON; it->flags = 1; it->x = (short)bx_x; it->y = top_y; it->w = (short)(w + hor); it->h = (short)(in.y - top_y); it->ref = node;
        }
    }
    if (marker_idx >= 0 && L.mark_item == marker_idx) { RD_ITEMS[marker_idx].y = content_top; L.mark_item = -1; }
    L.link = saved_link;
    int flush_bot = visible || pb > 0 || bb > 0 || has_h;
    int content_end = in.y, carry = 0;
    if (flush_bot) { content_end = in.y + in.pend; in.pend = 0; } else carry = in.pend;
    int chh = content_end - content_top;
    if (!rk) {
        if (hd >= 0) chh = hd;
        if (chh < minh) chh = minh;
        if (!css_len_is_auto(st->maxh) && st->maxh.pct == 0 && st->overflow_hidden) { int m = st->maxh.px; if (st->box_sizing_border) m -= ver; if (chh > m) chh = imax(0, m); }
    } else chh = ch;
    int end_y = content_top + chh + pb + bb;
    dl_box_fill(bx, bx_x, top_y, w + hor, end_y - top_y, st, 0);
    pc->y = end_y;
    pc->pend = flush_bot ? mb : mcombine(carry, mb);
}

/* ============================================================
 * scratch memory (a bump allocator that tables, flex and grid borrow from)
 * ============================================================ */
static inline void *lay_scratch(u32 bytes) {
    bytes = (bytes + 7u) & ~7u;
    if (L.scratch_top + bytes > (u32)RD_SCRATCH_SZ) return 0;
    void *p = RD_SCRATCH + L.scratch_top;
    L.scratch_top += bytes;
    return p;
}

/* ============================================================
 * content dispatcher + forced-size boxes (flex items and grid cells)
 * ============================================================ */
static void lay_content(u32 node, const css_style_t *st, lbox_t *in) {
    switch (st->display) {
        case DISP_FLEX: case DISP_INLINE_FLEX: lay_flex_content(node, st, in); break;
        case DISP_GRID: lay_grid_content(node, st, in); break;
        default: lay_children(node, st, in); break;
    }
}

/* Lays `node` out as a block whose content box is exactly content_w wide (and, if force_h >= 0, whose border box -
 * margins excluded - is exactly force_h tall) with its outer-left edge at x. Returns its height including margins. */
static int lay_forced(u32 node, const css_style_t *st, int x, int y, int content_w, int force_h, int ref_w) {
    css_style_t c2 = *st;
    int mt = rs(st->margin[0], ref_w), mb = rs(st->margin[2], ref_w), ml = rs(st->margin[3], ref_w), mr = rs(st->margin[1], ref_w);
    int hor = rs(st->padding[3], ref_w) + rs(st->padding[1], ref_w) + st->bw[3] + st->bw[1];
    if (c2.display == DISP_INLINE || c2.display == DISP_INLINE_BLOCK || c2.display == DISP_LIST_ITEM + 99) c2.display = DISP_BLOCK;
    if (c2.display == DISP_INLINE_FLEX) c2.display = DISP_FLEX;
    if (c2.display == DISP_INLINE_TABLE) c2.display = DISP_TABLE;
    if (c2.display == DISP_TABLE_CELL || c2.display == DISP_TABLE_ROW || c2.display == DISP_TABLE_GROUP || c2.display == DISP_TABLE_CAPTION) c2.display = DISP_BLOCK;
    c2.floating = 0; c2.minw = c2.maxw = css_len_auto();
    c2.margin[3] = css_len_px(ml); c2.margin[1] = css_len_px(mr); c2.margin[0] = css_len_px(mt); c2.margin[2] = css_len_px(mb);
    c2.box_sizing_border = 0;
    c2.width = css_len_px(content_w);
    if (force_h >= 0) { c2.box_sizing_border = 1; c2.width = css_len_px(content_w + hor); c2.height = css_len_px(imax(0, force_h - mt - mb)); c2.minh = css_len_auto(); }
    lbox_t pb; pb.x = x; pb.w = content_w + hor + ml + mr; pb.y = y; pb.pend = 0;
    lay_block(node, &c2, &pb);
    return pb.y + pb.pend - y;
}

/* lays a bare text node (an anonymous flex/grid item) into a box of width w; returns its height */
static int lay_text_box(u32 node, const css_style_t *pst, int x, int y, int w) {
    lbox_t cin; cin.x = x; cin.w = w; cin.y = y; cin.pend = 0;
    lb_t saved = LB;
    LB.x0 = x; LB.w = w; LB.cur = &cin; LB.fbase = saved.fbase + saved.nf; LB.nf = 0; LB.active = 0; LB.space = 0; LB.pen = 0; LB.first_line = 1;
    LB.indent = 0; LB.text_align = pst->text_align;
    int lh = st_line_height(pst); LB.strut_h = lh; LB.strut_asc = st_asc(pst, lh);
    lay_text(node, pst);
    lb_flush();
    LB = saved;
    return cin.y - y;
}

static inline int lay_text_is_blank(u32 node) {
    rd_node_t *t = &RD_NODES[node];
    for (u32 i = 0; i < t->b; i++) if (!dom_is_ws(RD_POOL[t->a + i])) return 0;
    return 1;
}

/* ============================================================
 * flexbox
 * ============================================================ */
#define FX_MAX 96
typedef struct { u32 node; int base, mn, ex, grow, shrink, w, x, h, i0, i1, align, is_text; } fxitem_t;

static void lay_flex_content(u32 node, const css_style_t *st, lbox_t *in) {
    in->y += in->pend; in->pend = 0;
    u32 mark = L.scratch_top;
    fxitem_t *it = (fxitem_t *)lay_scratch(sizeof(fxitem_t) * FX_MAX);
    if (!it) { lay_children(node, st, in); return; }
    int n = 0, cw = in->w, gapc = imin(imax(st->gap_col, 0), 2000), gapr = imin(imax(st->gap_row, 0), 2000), col = st->flex_dir == FD_COLUMN;
    for (u32 c = RD_NODES[node].first; c && n < FX_MAX; c = RD_NODES[c].next) {
        rd_node_t *cn = &RD_NODES[c];
        fxitem_t *f = &it[n];
        css_zero(f, sizeof(*f));
        f->node = c;
        if (cn->kind == RDK_TEXT) {
            if (lay_text_is_blank(c)) continue;
            intr_t a = { 0, 0, 0, 0 }; intr_text(c, st, &a); intr_flush(&a);
            f->is_text = 1; f->base = imin(a.mx, cw); f->mn = a.mn; f->shrink = 10; f->align = st->align_items;
            n++; continue;
        }
        if (cn->kind != RDK_ELEM) continue;
        css_style_t cs; css_compute(c, st, &cs);
        if (cs.display == DISP_NONE) continue;
        int l, r; lay_box_extras(&cs, cw, &l, &r);
        int hor = rs(cs.padding[3], cw) + rs(cs.padding[1], cw) + cs.bw[3] + cs.bw[1];
        f->ex = l + r;
        int rcw, rch, rctl, rk = lay_replaced_size(c, &cs, cw, &rcw, &rch, &rctl);
        int mn, mx;
        if (rk) { mn = mx = rcw; } else lay_intrinsic(c, &cs, &mn, &mx);
        int base;
        if (!css_len_is_auto(cs.flex_basis)) { base = css_len_resolve(cs.flex_basis, cw); if (cs.box_sizing_border) base -= hor; }
        else if (!css_len_is_auto(cs.width)) { base = css_len_resolve(cs.width, cw); if (cs.box_sizing_border) base -= hor; }
        else base = mx;
        f->base = imin(imax(0, base), 4000); f->mn = imin(mn, 4000); f->grow = imin(cs.flex_grow, 400); f->shrink = imin(cs.flex_shrink, 40);
        f->align = cs.align_self != AI_AUTO ? cs.align_self : st->align_items;
        n++;
    }
    int y = in->y;
    if (col) {
        for (int i = 0; i < n && !L.full; i++) {
            fxitem_t *f = &it[i];
            if (f->is_text) { y += lay_text_box(f->node, st, in->x, y, cw); }
            else {
                css_style_t cs; css_compute(f->node, st, &cs);
                int w = cw - f->ex - (rs(cs.padding[3], cw) + rs(cs.padding[1], cw) + cs.bw[3] + cs.bw[1]);
                int x = in->x;
                if (f->align == AI_CENTER || f->align == AI_END || f->align == AI_START) {
                    int hor = f->ex + (cw - f->ex - w);
                    int ww = imin(f->base > 0 ? f->base : f->mn, w);
                    if (!css_len_is_auto(cs.width)) ww = imin(f->base, w);
                    int free_w = cw - (ww + hor);
                    if (f->align == AI_CENTER) x += imax(0, free_w / 2); else if (f->align == AI_END) x += imax(0, free_w);
                    w = ww;
                }
                y += lay_forced(f->node, &cs, x, y, imax(0, w), -1, cw);
            }
            if (i + 1 < n) y += gapr;
        }
        in->y = y;
        L.scratch_top = mark;
        return;
    }
    int i = 0;
    while (i < n && !L.full) {
        int sum = 0, j = i;
        while (j < n) {
            int add = it[j].base + it[j].ex + (j > i ? gapc : 0);
            if (st->flex_wrap && j > i && sum + add > cw) break;
            sum += add; j++;
        }
        int cnt = j - i, free_w = cw - sum;
        int tg = 0, ts = 0;
        for (int k = i; k < j; k++) { tg += it[k].grow; ts += it[k].shrink * it[k].base; it[k].w = it[k].base; }
        if (free_w > 0 && tg > 0) { for (int k = i; k < j; k++) it[k].w = it[k].base + free_w * it[k].grow / tg; free_w = 0; }
        else if (free_w < 0 && ts > 0 && !st->flex_wrap) {
            int need = imin(-free_w, 4000);
            for (int k = i; k < j; k++) {
                int cut = need * (it[k].shrink * it[k].base) / ts;
                it[k].w = imax(imin(it[k].mn, it[k].base), it[k].base - cut);
            }
            int used = 0; for (int k = i; k < j; k++) used += it[k].w + it[k].ex + (k > i ? gapc : 0);
            free_w = imax(0, cw - used);
        }
        int lead = 0, between = gapc;
        if (free_w > 0) {
            switch (st->justify) {
                case JC_END: lead = free_w; break;
                case JC_CENTER: lead = free_w / 2; break;
                case JC_BETWEEN: if (cnt > 1) between = gapc + free_w / (cnt - 1); break;
                case JC_AROUND: lead = free_w / (2 * cnt); between = gapc + free_w / cnt; break;
                case JC_EVENLY: lead = free_w / (cnt + 1); between = gapc + free_w / (cnt + 1); break;
                default: break;
            }
        }
        int x = in->x + lead, line_h = 0, line_i0 = (int)L.n;
        for (int k = i; k < j; k++) { it[k].x = x; x += it[k].w + it[k].ex + between; }
        int force_h = -1;
        for (int pass = 0; pass < 2; pass++) {
            line_h = 0;
            for (int k = i; k < j; k++) {
                fxitem_t *f = &it[k];
                f->i0 = (int)L.n;
                if (f->is_text) f->h = lay_text_box(f->node, st, f->x, y, f->w);
                else {
                    css_style_t cs; css_compute(f->node, st, &cs);
                    int fh = (pass == 1 && f->align == AI_STRETCH && css_len_is_auto(cs.height)) ? force_h : -1;
                    f->h = lay_forced(f->node, &cs, f->x, y, f->w, fh, cw);
                }
                f->i1 = (int)L.n;
                if (f->h > line_h) line_h = f->h;
            }
            if (pass == 0) {                      /* uneven heights: lay the stretching items out again at the line's height */
                int need = 0;
                for (int k = i; k < j; k++) if (it[k].align == AI_STRETCH && !it[k].is_text && it[k].h < line_h) need = 1;
                if (!need || L.full) break;
                force_h = line_h; L.n = (u32)line_i0; L.mark_item = -1;
            }
        }
        for (int k = i; k < j; k++) {
            fxitem_t *f = &it[k];
            int dy = 0;
            if (f->align == AI_CENTER) dy = (line_h - f->h) / 2; else if (f->align == AI_END) dy = line_h - f->h;
            if (dy > 0) dl_translate(f->i0, f->i1, 0, dy);
        }
        y += line_h;
        if (j < n) y += gapr;
        i = j;
    }
    in->y = y; in->pend = 0;
    L.scratch_top = mark;
}

/* ============================================================
 * grid (the useful 80%: column tracks, auto-placement, gaps)
 * ============================================================ */
#define GR_MAXT 24
enum { GK_PX = 0, GK_PCT, GK_FR, GK_AUTO };

static int gr_tokens(const u8 *s, u32 n, int cw, int gap, int fpx, int *kind, int *val, int nt) {
    const u8 *p = s, *e = s + n, *ts; u32 tn;
    while (css_tok(&p, e, &ts, &tn) && nt < GR_MAXT) {
        if (ts[0] == ',' || ts[0] == '/') continue;
        if (tn > 8 && css_kw(ts, 7, "repeat(")) {
            const u8 *in = ts + 7; u32 il = tn - 8, k = 0, depth = 0;
            while (k < il && !(in[k] == ',' && depth == 0)) { if (in[k] == '(') depth++; else if (in[k] == ')') depth--; k++; }
            int count = 1;
            const u8 *tpl = in + (k < il ? k + 1 : il); u32 tl = k < il ? il - k - 1 : 0;
            if (css_kw(in, k, "auto-fit") || css_kw(in, k, "auto-fill")) {
                int minpx = 100; css_len_t l;
                const u8 *q = tpl; u32 ql = tl;
                while (ql && css_is_ws(q[0])) { q++; ql--; }
                if (ql > 7 && css_kw(q, 7, "minmax(")) { q += 7; ql -= 7; u32 m = 0; while (m < ql && q[m] != ',') m++; ql = m; }
                if (css_parse_len(q, ql, &l, fpx, 0) && !l.pct && !css_len_is_auto(l)) minpx = imax(1, l.px);
                count = imax(1, (cw + gap) / (minpx + gap));
            } else { int x; if (css_num(in, k, &x)) count = imax(1, x / 100); }
            int tk[GR_MAXT], tv[GR_MAXT];
            int tc = gr_tokens(tpl, tl, cw, gap, fpx, tk, tv, 0);
            for (int r = 0; r < count && nt < GR_MAXT; r++)
                for (int q = 0; q < tc && nt < GR_MAXT; q++) { kind[nt] = tk[q]; val[nt] = tv[q]; nt++; }
            continue;
        }
        if (tn > 8 && css_kw(ts, 7, "minmax(")) {
            const u8 *in = ts + 7; u32 il = tn - 8, k = 0, depth = 0;
            while (k < il && !(in[k] == ',' && depth == 0)) { if (in[k] == '(') depth++; else if (in[k] == ')') depth--; k++; }
            if (k < il) nt = gr_tokens(in + k + 1, il - k - 1, cw, gap, fpx, kind, val, nt);
            continue;
        }
        int x; u32 c = css_num(ts, tn, &x);
        if (c && css_kw(ts + c, tn - c, "fr")) { kind[nt] = GK_FR; val[nt] = imax(10, x); nt++; continue; }
        css_len_t l;
        if (css_parse_len(ts, tn, &l, fpx, 0) && !css_len_is_auto(l)) {
            if (l.pct) { kind[nt] = GK_PCT; val[nt] = l.pct; } else { kind[nt] = GK_PX; val[nt] = l.px; }
        } else { kind[nt] = GK_AUTO; val[nt] = 100; }
        nt++;
    }
    return nt;
}

static void lay_grid_content(u32 node, const css_style_t *st, lbox_t *in) {
    in->y += in->pend; in->pend = 0;
    u32 mark = L.scratch_top;
    fxitem_t *it = (fxitem_t *)lay_scratch(sizeof(fxitem_t) * FX_MAX);
    if (!it) { lay_children(node, st, in); return; }
    int cw = in->w, gapc = imin(imax(st->gap_col, 0), 2000), gapr = imin(imax(st->gap_row, 0), 2000);
    int kind[GR_MAXT], val[GR_MAXT], colw[GR_MAXT], colx[GR_MAXT + 1], nt = 0;
    if (st->gtc_len) nt = gr_tokens(RD_POOL + st->gtc_off, st->gtc_len, cw, gapc, st->fpx, kind, val, 0);
    if (nt == 0) { kind[0] = GK_FR; val[0] = 100; nt = 1; }
    int fixed = 0, frs = 0;
    for (int c = 0; c < nt; c++) {
        if (kind[c] == GK_PX) fixed += val[c];
        else if (kind[c] == GK_PCT) fixed += cw * val[c] / 1000;
        else frs += kind[c] == GK_FR ? val[c] : 100;
    }
    int room = imax(0, cw - fixed - gapc * (nt - 1));
    for (int c = 0; c < nt; c++) {
        if (kind[c] == GK_PX) colw[c] = val[c];
        else if (kind[c] == GK_PCT) colw[c] = cw * val[c] / 1000;
        else colw[c] = frs ? room * (kind[c] == GK_FR ? val[c] : 100) / frs : 0;
    }
    colx[0] = in->x;
    for (int c = 0; c < nt; c++) colx[c + 1] = colx[c] + colw[c] + gapc;
    int n = 0;
    for (u32 c = RD_NODES[node].first; c && n < FX_MAX; c = RD_NODES[c].next) {
        rd_node_t *cn = &RD_NODES[c];
        fxitem_t *f = &it[n]; css_zero(f, sizeof(*f)); f->node = c;
        if (cn->kind == RDK_TEXT) { if (lay_text_is_blank(c)) continue; f->is_text = 1; f->align = AI_STRETCH; n++; continue; }
        if (cn->kind != RDK_ELEM) continue;
        css_style_t cs; css_compute(c, st, &cs);
        if (cs.display == DISP_NONE) continue;
        int l, r; lay_box_extras(&cs, cw, &l, &r);
        f->ex = l + r;
        f->align = cs.align_self != AI_AUTO ? cs.align_self : st->align_items;
        n++;
    }
    int y = in->y, i = 0;
    while (i < n && !L.full) {
        int j = imin(n, i + nt), row_h = 0, row_i0 = (int)L.n, force_h = -1;
        for (int pass = 0; pass < 2; pass++) {
            row_h = 0;
            for (int k = i; k < j; k++) {
                fxitem_t *f = &it[k]; int col = k - i;
                f->i0 = (int)L.n;
                int w = imax(0, colw[col] - f->ex);
                if (f->is_text) f->h = lay_text_box(f->node, st, colx[col], y, colw[col]);
                else {
                    css_style_t cs; css_compute(f->node, st, &cs);
                    int fh = (pass == 1 && f->align == AI_STRETCH && css_len_is_auto(cs.height)) ? force_h : -1;
                    if (!css_len_is_auto(cs.width) && f->align != AI_STRETCH) w = imin(w, css_len_resolve(cs.width, cw));
                    f->h = lay_forced(f->node, &cs, colx[col], y, w, fh, cw);
                }
                f->i1 = (int)L.n;
                if (f->h > row_h) row_h = f->h;
            }
            if (pass == 0) {
                int need = 0;
                for (int k = i; k < j; k++) if (it[k].align == AI_STRETCH && !it[k].is_text && it[k].h < row_h) need = 1;
                if (!need || L.full) break;
                force_h = row_h; L.n = (u32)row_i0; L.mark_item = -1;
            }
        }
        for (int k = i; k < j; k++) {
            int dy = 0;
            if (it[k].align == AI_CENTER) dy = (row_h - it[k].h) / 2; else if (it[k].align == AI_END) dy = row_h - it[k].h;
            if (dy > 0) dl_translate(it[k].i0, it[k].i1, 0, dy);
        }
        y += row_h;
        if (j < n) y += gapr;
        i = j;
    }
    in->y = y;
    L.scratch_top = mark;
}

/* ============================================================
 * tables
 * ============================================================ */
#define TB_MAXC   900
#define TB_MAXR   360
#define TB_MAXCOL 48

typedef struct {
    u32 node;
    short row, col, cs, rs, end_row, cmin, cmax, fixed, pct;
    int top, h, i0, i1;
    dl_box_t bx;
} tbcell_t;
typedef struct { u32 node, group; } tbrow_t;

static inline void tb_row_style(const css_style_t *tst, const tbrow_t *rw, css_style_t *rst) {
    if (rw->group) { css_style_t gs; css_compute(rw->group, tst, &gs); css_compute(rw->node, &gs, rst); }
    else css_compute(rw->node, tst, rst);
}

static void lay_table(u32 node, const css_style_t *st, lbox_t *pc) {
    int avail = pc->w;
    int mt = rs(st->margin[0], avail), mb = rs(st->margin[2], avail);
    int ml_auto = css_len_is_auto(st->margin[3]), mr_auto = css_len_is_auto(st->margin[1]);
    int ml = rs(st->margin[3], avail), mr = rs(st->margin[1], avail);
    int pt = rs(st->padding[0], avail), pr = rs(st->padding[1], avail), pb = rs(st->padding[2], avail), pl = rs(st->padding[3], avail);
    int bt = st->bw[0], br = st->bw[1], bb = st->bw[2], bl = st->bw[3];
    int tbh = pl + pr + bl + br;
    u32 mark = L.scratch_top;
    tbrow_t *rows = (tbrow_t *)lay_scratch(TB_MAXR * sizeof(tbrow_t));
    tbcell_t *cells = (tbcell_t *)lay_scratch(TB_MAXC * sizeof(tbcell_t));
    if (!rows || !cells) { L.scratch_top = mark; css_style_t b2 = *st; b2.display = DISP_BLOCK; lay_block(node, &b2, pc); return; }

    /* ---- 1. find the rows and cells, place them on the grid ---- */
    int nr = 0, nc = 0, ncols = 0; u32 caption = 0;
    short colbusy[TB_MAXCOL]; for (int i = 0; i < TB_MAXCOL; i++) colbusy[i] = 0;
    for (u32 g = RD_NODES[node].first; g; g = RD_NODES[g].next) {
        if (RD_NODES[g].kind != RDK_ELEM) continue;
        if (RD_NODES[g].tag == TG_CAPTION) { if (!caption) caption = g; continue; }
        css_style_t gs; css_compute(g, st, &gs);
        if (gs.display == DISP_NONE) continue;
        int is_group = RD_NODES[g].tag == TG_THEAD || RD_NODES[g].tag == TG_TBODY || RD_NODES[g].tag == TG_TFOOT || gs.display == DISP_TABLE_GROUP;
        for (u32 r = is_group ? RD_NODES[g].first : g; r; r = is_group ? RD_NODES[r].next : 0) {
            if (RD_NODES[r].kind != RDK_ELEM || RD_NODES[r].tag != TG_TR) continue;
            if (nr >= TB_MAXR) break;
            css_style_t rsty;
            if (is_group) css_compute(r, &gs, &rsty); else rsty = gs;
            if (rsty.display == DISP_NONE) continue;
            rows[nr].node = r; rows[nr].group = is_group ? g : 0;
            int col = 0;
            for (u32 c = RD_NODES[r].first; c; c = RD_NODES[c].next) {
                if (RD_NODES[c].kind != RDK_ELEM || (RD_NODES[c].tag != TG_TD && RD_NODES[c].tag != TG_TH)) continue;
                css_style_t cs; css_compute(c, &rsty, &cs);
                if (cs.display == DISP_NONE || nc >= TB_MAXC) continue;
                int span = imin(imax(lay_attr_int(c, AT_COLSPAN, 1), 1), TB_MAXCOL), rspan = imin(imax(lay_attr_int(c, AT_ROWSPAN, 1), 1), 255);
                while (col < TB_MAXCOL && colbusy[col] > nr) col++;
                if (col >= TB_MAXCOL) break;
                if (col + span > TB_MAXCOL) span = TB_MAXCOL - col;
                tbcell_t *tc = &cells[nc++];
                tc->node = c; tc->row = (short)nr; tc->col = (short)col; tc->cs = (short)span; tc->rs = (short)rspan;
                for (int k = 0; k < span; k++) colbusy[col + k] = (short)(nr + rspan);
                int chor = rs(cs.padding[3], 0) + rs(cs.padding[1], 0) + cs.bw[3] + cs.bw[1], mn, mx;
                lay_intrinsic(c, &cs, &mn, &mx);
                tc->cmin = (short)imin(mn + chor, 30000); tc->cmax = (short)imin(mx + chor, 30000);
                tc->fixed = 0; tc->pct = 0;
                if (!css_len_is_auto(cs.width)) {
                    if (cs.width.pct) tc->pct = cs.width.pct; else tc->fixed = (short)imin(imax(0, cs.width.px + (cs.box_sizing_border ? 0 : chor)), 4000);
                }
                col += span; if (col > ncols) ncols = col;
            }
            nr++;
        }
    }
    for (int i = 0; i < nc; i++) { int e = cells[i].row + cells[i].rs - 1; cells[i].end_row = (short)(e >= nr ? nr - 1 : e); }

    /* ---- 2. column widths ---- */
    short colmin[TB_MAXCOL], colmax[TB_MAXCOL], colfix[TB_MAXCOL], colpct[TB_MAXCOL], colw[TB_MAXCOL];
    int colx[TB_MAXCOL + 1];
    for (int i = 0; i < TB_MAXCOL; i++) colmin[i] = colmax[i] = colfix[i] = colpct[i] = colw[i] = 0;
    int spx = st->border_collapse ? 0 : st->spacing_x, spy = st->border_collapse ? 0 : st->spacing_y;
    for (int i = 0; i < nc; i++) {
        tbcell_t *c = &cells[i];
        if (c->cs != 1) continue;
        if (c->cmin > colmin[c->col]) colmin[c->col] = c->cmin;
        if (c->cmax > colmax[c->col]) colmax[c->col] = c->cmax;
        if (c->fixed > colfix[c->col]) colfix[c->col] = c->fixed;
        if (c->pct > colpct[c->col]) colpct[c->col] = c->pct;
    }
    for (int i = 0; i < nc; i++) {                                    /* spanning cells: spread what the singles can't cover */
        tbcell_t *c = &cells[i];
        if (c->cs == 1) continue;
        int smin = 0, smax = 0;
        for (int k = 0; k < c->cs; k++) { smin += colmin[c->col + k]; smax += colmax[c->col + k]; }
        smin += spx * (c->cs - 1); smax += spx * (c->cs - 1);
        if (c->cmin > smin) { int d = c->cmin - smin; for (int k = 0; k < c->cs; k++) colmin[c->col + k] += (short)(d / c->cs + (k == c->cs - 1 ? d % c->cs : 0)); }
        if (c->cmax > smax) { int d = c->cmax - smax; for (int k = 0; k < c->cs; k++) colmax[c->col + k] += (short)(d / c->cs + (k == c->cs - 1 ? d % c->cs : 0)); }
    }
    int any_pct = 0, summin = 0, summax = 0;
    for (int c = 0; c < ncols; c++) {
        if (colmax[c] < colmin[c]) colmax[c] = colmin[c];
        if (colfix[c] > 0) colmax[c] = (short)imax(colmin[c], colfix[c]);
        if (colpct[c]) any_pct = 1;
        summin += colmin[c]; summax += colmax[c];
    }
    int sumspace = spx * (ncols + 1);
    int declared = !css_len_is_auto(st->width) || any_pct;
    int max_outer = avail - (ml_auto ? 0 : ml) - (mr_auto ? 0 : mr);
    int W;
    if (!css_len_is_auto(st->width)) { W = css_len_resolve(st->width, avail); if (!st->box_sizing_border && !st->width.pct) W += tbh; }
    else if (any_pct) W = max_outer;
    else W = summax + sumspace + tbh;
    if (W > max_outer) W = max_outer;
    if (W < summin + sumspace + tbh) W = summin + sumspace + tbh;
    int target = imax(0, W - tbh - sumspace);
    if (any_pct) {
        summax = 0;
        for (int c = 0; c < ncols; c++) { if (colpct[c]) colmax[c] = (short)imax(colmin[c], target * colpct[c] / 1000); summax += colmax[c]; }
    }
    if (target >= summax) {
        int extra = target - summax;
        for (int c = 0; c < ncols; c++) colw[c] = colmax[c] + (declared && extra > 0 ? (summax > 0 ? extra * colmax[c] / summax : extra / imax(1, ncols)) : 0);
    } else if (target > summin) {
        int span = summax - summin;
        for (int c = 0; c < ncols; c++) colw[c] = (short)(colmin[c] + (colmax[c] - colmin[c]) * (target - summin) / span);
    } else for (int c = 0; c < ncols; c++) colw[c] = colmin[c];
    if (ncols > 0 && target >= summin && (declared || target < summax)) {
        int tot = 0; for (int c = 0; c < ncols; c++) tot += colw[c];
        if (tot != target) colw[ncols - 1] = (short)imax(colmin[ncols - 1], colw[ncols - 1] + target - tot);
    }
    int real_tot = 0; for (int c = 0; c < ncols; c++) real_tot += colw[c];
    W = real_tot + sumspace + tbh;

    int free_w = avail - W - ml - mr;
    if (free_w > 0) {
        if (ml_auto && mr_auto) { ml += free_w / 2; mr += free_w - free_w / 2; }
        else if (ml_auto) ml += free_w; else if (mr_auto) mr += free_w;
    }

    /* ---- 3. the box and its rows ---- */
    pc->y += mcombine(pc->pend, mt); pc->pend = 0;
    int top_y = pc->y, tx = pc->x + ml;
    dl_box_t bx = dl_box_reserve(st);
    int y = top_y + bt + pt;
    if (caption) {
        css_style_t cst; css_compute(caption, st, &cst);
        if (cst.display != DISP_NONE) {
            cst.display = DISP_BLOCK;
            lbox_t cin; cin.x = tx + bl + pl; cin.w = imax(0, W - tbh); cin.y = y; cin.pend = 0;
            lay_block(caption, &cst, &cin);
            y = cin.y + cin.pend;
        }
    }
    y += spy;
    colx[0] = tx + bl + pl + spx;
    for (int c = 0; c < ncols; c++) colx[c + 1] = colx[c] + colw[c] + spx;
    int ci = 0;
    for (int r = 0; r < nr && !L.full; r++) {
        css_style_t rsty; tb_row_style(st, &rows[r], &rsty);
        int rbg = -1;
        if (rsty.bg != CSS_NOCOLOR) { rbg = (int)L.n; rd_item_t *ri = dl_new(); ri->kind = DL_NONE; }
        int row_h = 0;
        for (; ci < nc && cells[ci].row == r; ci++) {
            tbcell_t *tc = &cells[ci];
            css_style_t cs; css_compute(tc->node, &rsty, &cs);
            int cwtot = spx * (tc->cs - 1);
            for (int k = 0; k < tc->cs; k++) cwtot += colw[tc->col + k];
            int cpl = rs(cs.padding[3], cwtot), cpr = rs(cs.padding[1], cwtot), cpt = rs(cs.padding[0], cwtot), cpb = rs(cs.padding[2], cwtot);
            int chor = cpl + cpr + cs.bw[3] + cs.bw[1], cver = cpt + cpb + cs.bw[0] + cs.bw[2];
            tc->bx = dl_box_reserve(&cs);
            tc->top = y;
            lbox_t in; in.x = colx[tc->col] + cs.bw[3] + cpl; in.w = imax(0, cwtot - chor); in.y = y + cs.bw[0] + cpt; in.pend = 0;
            tc->i0 = (int)L.n;
            u32 saved_link = L.link;
            lay_children(tc->node, &cs, &in);
            L.link = saved_link;
            tc->i1 = (int)L.n;
            int h = in.y + in.pend - (y + cs.bw[0] + cpt) + cver;
            if (!css_len_is_auto(cs.height) && cs.height.pct == 0) h = imax(h, cs.height.px + (cs.box_sizing_border ? 0 : cver));
            tc->h = h;
            if (tc->rs == 1 && h > row_h) row_h = h;
        }
        if (!css_len_is_auto(rsty.height) && rsty.height.pct == 0 && rsty.height.px > row_h) row_h = rsty.height.px;
        for (int i = 0; i < nc; i++) {                                /* cells that end on this row (including rowspans from above) */
            tbcell_t *tc = &cells[i];
            if (tc->end_row != r || tc->row > r) continue;
            int need = tc->h - ((y + row_h) - tc->top);
            if (need > 0) row_h += need;
        }
        for (int i = 0; i < nc; i++) {
            tbcell_t *tc = &cells[i];
            if (tc->end_row != r || tc->row > r) continue;
            css_style_t crs, cs;
            if (tc->row == r) crs = rsty; else tb_row_style(st, &rows[tc->row], &crs);
            css_compute(tc->node, &crs, &cs);
            int cwtot = spx * (tc->cs - 1);
            for (int k = 0; k < tc->cs; k++) cwtot += colw[tc->col + k];
            int total = (y + row_h) - tc->top;
            int skip = 0;
            if (st->border_collapse) { if (tc->col + tc->cs < ncols) skip |= 2; if (tc->end_row < nr - 1) skip |= 4; }
            u32 saved_link = L.link; (void)saved_link;
            dl_box_fill(tc->bx, colx[tc->col], tc->top, cwtot, total, &cs, skip);
            int extra = total - tc->h, dy = 0;
            if (cs.valign == VA_BOTTOM) dy = extra; else if (cs.valign == VA_MIDDLE || cs.valign == VA_BASELINE) dy = extra / 2;
            if (dy > 0) dl_translate(tc->i0, tc->i1, 0, dy);
        }
        if (rbg >= 0 && !L.full) {
            rd_item_t *ri = &RD_ITEMS[rbg];
            ri->kind = DL_RECT; ri->x = (short)colx[0]; ri->y = y; ri->w = (short)imax(0, colx[ncols] - spx - colx[0]); ri->h = (short)row_h; ri->color = rsty.bg;
            if (ri->w <= 0 || ri->h <= 0) ri->kind = DL_NONE;
        }
        y += row_h + spy;
    }
    int end_y = y + pb + bb;
    if (!css_len_is_auto(st->height) && st->height.pct == 0) { int hh = st->height.px + (st->box_sizing_border ? 0 : pt + pb + bt + bb); if (end_y - top_y < hh) end_y = top_y + hh; }
    dl_box_fill(bx, tx, top_y, W, end_y - top_y, st, 0);
    pc->y = end_y; pc->pend = mb;
    L.last_w = ml + W + mr;
    L.scratch_top = mark;
}

/* ============================================================
 * the whole document
 * ============================================================ */
/* Lays out the parsed document for a viewport `vw` pixels wide. Returns the document height. */
static int rd_layout(int vw, int vh) {
    css_viewport_w = vw; css_viewport_h = vh;
    L.n = 0; L.full = 0; L.link = 0; L.mark_item = -1; L.depth = 0; L.scratch_top = 0; L.last_w = 0;
    dom_pool_len = L.pool_mark;
    for (u32 i = 0; i < dom_node_count; i++) RD_NODES[i].flags &= (u16)~1u;
    css_load_document();
    css_zero(&LB, sizeof(LB));
    css_style_t root, body;
    css_compute(dom_html, 0, &root);
    css_compute(dom_body, &root, &body);
    L.canvas_bg = root.bg != CSS_NOCOLOR ? root.bg : (body.bg != CSS_NOCOLOR ? body.bg : 0xFFFFFFu);
    root.display = DISP_BLOCK;
    lbox_t top; top.x = 0; top.w = vw; top.y = 0; top.pend = 0;
    lay_block(dom_html, &root, &top);
    L.doc_h = top.y + top.pend;
    return L.doc_h;
}

#endif
