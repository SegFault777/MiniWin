#ifndef CSS_H
#define CSS_H
#include "dom.h"

/* ============================================================
 * css.h -- the middle of MiniWeb's HTML5 engine: stylesheets in,
 * "what does this element look like" out.
 *
 *   PARSING   <style> blocks, the built-in user-agent sheet, and style=""
 *             attributes: rules, selector lists, declarations, !important,
 *             @media (min/max-width, screen/print, prefers-color-scheme),
 *             @supports/@layer (looked inside, not evaluated), custom
 *             properties on :root/html/body with var(--x, fallback), and
 *             calc()/min()/max()/clamp() in the sizes people actually write.
 *   SELECTORS type, *, .class, #id, [attr], [attr=v] [~=] [|=] [^=] [$=] [*=],
 *             descendant, >, +, ~, :first-child :last-child :only-child
 *             :nth-child(odd|even|an+b) :not(simple) :root :empty :link
 *             :checked :disabled. The dynamic ones (:hover :focus :active
 *             :visited) and every ::pseudo-element simply never match --
 *             a static page has no pointer to hover with, and no
 *             generated content to put before or after.
 *   CASCADE   UA sheet < presentational attributes (<font color>,
 *             bgcolor, cellpadding...) < author rules by specificity and
 *             source order < style="" ; !important flips that order.
 *   UNITS     everything is converted to DEVICE pixels at the moment it is
 *             parsed. The browser runs at a 11/16 "zoom": 16 CSS px of body
 *             text is exactly one 11x11 glyph cell, so a 960px-wide site
 *             design fits a 640px screen the way it would in a browser
 *             zoomed out to 69%. Percentages stay percentages until layout.
 *
 * No malloc: rules, selector compounds and declarations live in fixed
 * arrays (kernel: memmap.h, host tests: plain arrays).
 * ============================================================ */

#ifndef RD_CSS_RULES
#define RD_CSS_RULE_MAX  MW_RD_CSS_RULE_MAX
#define RD_CSS_COMP_MAX  MW_RD_CSS_COMP_MAX
#define RD_CSS_DECL_MAX  MW_RD_CSS_DECL_MAX
#define RD_CSS_RULES ((css_rule_t *)MW_RD_CSS_RULES_ADDR)
#define RD_CSS_COMPS ((css_comp_t *)MW_RD_CSS_COMPS_ADDR)
#define RD_CSS_DECLS ((css_decl_t *)MW_RD_CSS_DECLS_ADDR)
#endif

/* ---------- the zoom: CSS px -> device px ---------- */
#define CSS_ZOOM_NUM 11
#define CSS_ZOOM_DEN 16
#define CSS_BASE_FONT_PX 16            /* the initial `medium` font size, in CSS px */
#define CSS_MIN_CELL 11                /* the smallest glyph cell we own */
#define CSS_MAX_CELL 66

static int css_viewport_w = 620;       /* device px, set by the renderer before each layout */
static int css_viewport_h = 400;

/* ---------- lengths ---------- */
#define CSS_AUTO 0x7FFF
typedef struct { short px; short pct; } css_len_t;       /* value = px + pct/10 percent of the containing width; pct==CSS_AUTO -> auto */
static inline css_len_t css_len_px(int px) { css_len_t l; l.px = (short)(px > 32000 ? 32000 : px < -32000 ? -32000 : px); l.pct = 0; return l; }
static inline css_len_t css_len_auto(void) { css_len_t l; l.px = 0; l.pct = CSS_AUTO; return l; }
static inline int css_len_is_auto(css_len_t l) { return l.pct == CSS_AUTO; }
/* resolves against the containing block's width `base` */
static inline int css_len_resolve(css_len_t l, int base) {
    if (l.pct == CSS_AUTO) return 0;
    if (base > 20000) base = 20000; else if (base < -20000) base = -20000;
    int v = l.px + (base * (int)l.pct) / 1000;
    return v > 20000 ? 20000 : v < -20000 ? -20000 : v;
}

/* ---------- colors: 0x00RRGGBB, or CSS_NOCOLOR for transparent ---------- */
#define CSS_NOCOLOR   0x80000000u
#define CSS_CURRENT   0x40000000u      /* "currentcolor", resolved while applying */

/* ---------- the computed style of one element ---------- */
enum { DISP_NONE = 0, DISP_INLINE, DISP_BLOCK, DISP_LIST_ITEM, DISP_INLINE_BLOCK, DISP_TABLE, DISP_INLINE_TABLE,
       DISP_TABLE_ROW, DISP_TABLE_CELL, DISP_TABLE_GROUP, DISP_TABLE_CAPTION, DISP_FLEX, DISP_INLINE_FLEX, DISP_GRID };
enum { TA_LEFT = 0, TA_CENTER, TA_RIGHT, TA_JUSTIFY };
enum { WS_NORMAL = 0, WS_NOWRAP, WS_PRE, WS_PRE_WRAP, WS_PRE_LINE };
enum { BS_NONE = 0, BS_SOLID, BS_DASHED, BS_DOTTED, BS_DOUBLE, BS_INSET, BS_OUTSET, BS_GROOVE, BS_RIDGE };
enum { VA_BASELINE = 0, VA_TOP, VA_MIDDLE, VA_BOTTOM, VA_SUB, VA_SUPER };
enum { LS_NONE = 0, LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_LOWER_ALPHA, LS_UPPER_ALPHA, LS_LOWER_ROMAN, LS_UPPER_ROMAN };
enum { TT_NONE = 0, TT_UPPER, TT_LOWER, TT_CAP };
enum { FD_ROW = 0, FD_COLUMN };
enum { JC_START = 0, JC_END, JC_CENTER, JC_BETWEEN, JC_AROUND, JC_EVENLY };
enum { AI_STRETCH = 0, AI_START, AI_END, AI_CENTER, AI_AUTO = 9 };
enum { POS_STATIC = 0, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED };
#define SF_BOLD   1
#define SF_ITALIC 2
#define SF_UNDER  4
#define SF_STRIKE 8

typedef struct {
    u8 display, position, floating, clear;
    u8 text_align, valign, white_space, visibility;
    u8 text_transform, list_style, overflow_hidden, box_sizing_border;
    u8 flags, cell, border_collapse, hidden_text;
    u8 bstyle[4], bw[4];                 /* top, right, bottom, left */
    u8 flex_dir, flex_wrap, justify, align_items, align_self, order;
    u8 lh_mode;                          /* 0 normal, 1 = lh is px, 2 = lh is a factor x100 */
    u8 pad8;
    short fpx;                           /* font-size in CSS px (what `em` multiplies) */
    short lh;
    short gap_row, gap_col, spacing_x, spacing_y, text_indent;
    short flex_grow, flex_shrink;        /* x10 */
    u32 color, bg, bcolor[4];
    css_len_t margin[4], padding[4];
    css_len_t width, height, minw, maxw, minh, maxh, flex_basis;
    u32 gtc_off; u16 gtc_len;            /* grid-template-columns text (pool offset) */
    u16 pad16;
} css_style_t;

/* ---------- rules, compounds, declarations ---------- */
#define PS_FIRST     0x0001
#define PS_LAST      0x0002
#define PS_ONLY      0x0004
#define PS_ROOT      0x0008
#define PS_EMPTY     0x0010
#define PS_LINK      0x0020
#define PS_CHECKED   0x0040
#define PS_DISABLED  0x0080
#define PS_NTH       0x0100
#define PS_NOT       0x0200

enum { AOP_NONE = 0, AOP_EXISTS, AOP_EQ, AOP_WORD, AOP_DASH, AOP_PREFIX, AOP_SUFFIX, AOP_SUBSTR };

typedef struct {
    u32 id_hash;
    u32 cls[2];
    u32 neg_val;
    u32 attr_voff;
    u16 tag;
    u16 pseudo;
    u16 attr_vlen;
    u8 ncls, comb;                       /* comb: the combinator joining this compound to the one on its left */
    u8 attr_id, attr_op;
    u8 neg_kind;                         /* 1 tag, 2 class, 3 id */
    signed char nth_a, nth_b;
    u8 attr_icase, pad;
} css_comp_t;

typedef struct {
    u32 comp_first, decl_first, spec, order;
    u16 ndecl, next;                     /* next: chain inside its lookup bucket */
    u8 ncomp, key_kind;
    u32 key;
} css_rule_t;

typedef struct { u16 prop; u8 imp, pad; u32 voff; u32 vlen; } css_decl_t;

#define CSS_BUCKET_N 256
static u32 css_rule_count, css_comp_count, css_decl_count, css_order;
static u16 css_by_tag[TG__COUNT];
static u16 css_by_cls[CSS_BUCKET_N];
static u16 css_by_id[CSS_BUCKET_N];
static u16 css_universal;
#define CSS_NIL 0xFFFF

/* ---------- small string helpers ---------- */
static inline int css_is_ws(u8 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static inline u8 css_lc(u8 c) { return (c >= 'A' && c <= 'Z') ? (u8)(c + 32) : c; }
static inline int css_is_digit(u8 c) { return c >= '0' && c <= '9'; }
static inline int css_is_ident(u8 c) { return dom_is_alnum(c) || c == '-' || c == '_' || c >= 0x80; }
/* case-insensitive compare of s[0..n) with a lower-case literal */
static inline int css_kw(const u8 *s, u32 n, const char *lit) {
    u32 i = 0;
    for (; lit[i]; i++) { if (i >= n || css_lc(s[i]) != (u8)lit[i]) return 0; }
    return i == n;
}
static inline u32 css_hash(const u8 *s, u32 n) {               /* FNV-1a, case-sensitive (class and id names are) */
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= s[i]; h *= 16777619u; }
    return h ? h : 1;
}
static inline u32 css_hash_lc(const u8 *s, u32 n) {
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= css_lc(s[i]); h *= 16777619u; }
    return h ? h : 1;
}

/* ---------- numbers: fixed point x100 ---------- */
/* Parses [+-]digits[.digits] at s[0..n); returns the chars consumed (0 = not a number). */
static inline u32 css_num(const u8 *s, u32 n, int *out100) {
    u32 i = 0; int neg = 0, v = 0, frac = 0, fd = 0, any = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
    while (i < n && css_is_digit(s[i])) { if (v < 1000) v = v * 10 + (s[i] - '0'); i++; any = 1; }     /* saturates near 9999: nobody needs more, and the math downstream stays in 32 bits */
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && css_is_digit(s[i])) { if (fd < 2) { frac = frac * 10 + (s[i] - '0'); fd++; } i++; any = 1; }
    }
    if (!any) return 0;
    while (fd < 2) { frac *= 10; fd++; }
    v = v * 100 + frac;
    *out100 = neg ? -v : v;
    return i;
}

/* ---------- custom properties (var(--x)) ---------- */
#define CSS_VAR_MAX 48
static u32 css_var_hash[CSS_VAR_MAX], css_var_off[CSS_VAR_MAX], css_var_len[CSS_VAR_MAX];
static u32 css_var_count;

static inline void css_var_set(u32 h, u32 off, u32 len) {
    for (u32 i = 0; i < css_var_count; i++) if (css_var_hash[i] == h) { css_var_off[i] = off; css_var_len[i] = len; return; }
    if (css_var_count < CSS_VAR_MAX) { css_var_hash[css_var_count] = h; css_var_off[css_var_count] = off; css_var_len[css_var_count] = len; css_var_count++; }
}
static inline int css_var_get(u32 h, u32 *off, u32 *len) {
    for (u32 i = 0; i < css_var_count; i++) if (css_var_hash[i] == h) { *off = css_var_off[i]; *len = css_var_len[i]; return 1; }
    return 0;
}

/* Expands every var(--name[, fallback]) in v[0..n) into out (NUL-terminated); returns the length. */
static inline u32 css_expand(const u8 *v, u32 n, u8 *out, u32 cap, int depth) {
    u32 o = 0;
    for (u32 i = 0; i < n && o + 1 < cap; ) {
        if (i + 4 <= n && css_lc(v[i]) == 'v' && css_lc(v[i + 1]) == 'a' && css_lc(v[i + 2]) == 'r' && v[i + 3] == '(' && depth < 4) {
            u32 j = i + 4, nest = 1;
            while (j < n && nest) { if (v[j] == '(') nest++; else if (v[j] == ')') nest--; j++; }
            u32 end = nest ? n : j - 1;                          /* index of the closing ')' */
            u32 ns = i + 4;
            while (ns < end && css_is_ws(v[ns])) ns++;
            u32 ne = ns;
            while (ne < end && v[ne] != ',' && !css_is_ws(v[ne])) ne++;
            u32 voff, vlen;
            if (css_var_get(css_hash(v + ns, ne - ns), &voff, &vlen)) {
                o += css_expand(RD_POOL + voff, vlen, out + o, cap - o, depth + 1);
            } else {
                u32 fb = ne;
                while (fb < end && (v[fb] != ',')) fb++;
                if (fb < end) { fb++; while (fb < end && css_is_ws(v[fb])) fb++; o += css_expand(v + fb, end - fb, out + o, cap - o, depth + 1); }
            }
            i = nest ? n : j;
            continue;
        }
        out[o++] = v[i++];
    }
    out[o] = 0;
    return o;
}

/* ---------- colors ---------- */
typedef struct { const char *name; u32 rgb; } css_namedcolor_t;
static const css_namedcolor_t css_named[] = {
    {"black",0x000000},{"white",0xFFFFFF},{"red",0xFF0000},{"green",0x008000},{"blue",0x0000FF},{"yellow",0xFFFF00},
    {"gray",0x808080},{"grey",0x808080},{"silver",0xC0C0C0},{"maroon",0x800000},{"purple",0x800080},{"fuchsia",0xFF00FF},
    {"magenta",0xFF00FF},{"lime",0x00FF00},{"olive",0x808000},{"navy",0x000080},{"teal",0x008080},{"aqua",0x00FFFF},
    {"cyan",0x00FFFF},{"orange",0xFFA500},{"pink",0xFFC0CB},{"brown",0xA52A2A},{"gold",0xFFD700},{"tan",0xD2B48C},
    {"beige",0xF5F5DC},{"ivory",0xFFFFF0},{"khaki",0xF0E68C},{"coral",0xFF7F50},{"salmon",0xFA8072},{"crimson",0xDC143C},
    {"indigo",0x4B0082},{"violet",0xEE82EE},{"orchid",0xDA70D6},{"plum",0xDDA0DD},{"lavender",0xE6E6FA},{"azure",0xF0FFFF},
    {"turquoise",0x40E0D0},{"chocolate",0xD2691E},{"firebrick",0xB22222},{"forestgreen",0x228B22},{"darkgreen",0x006400},
    {"darkblue",0x00008B},{"darkred",0x8B0000},{"darkgray",0xA9A9A9},{"darkgrey",0xA9A9A9},{"lightgray",0xD3D3D3},
    {"lightgrey",0xD3D3D3},{"lightblue",0xADD8E6},{"lightgreen",0x90EE90},{"lightyellow",0xFFFFE0},{"lightpink",0xFFB6C1},
    {"dimgray",0x696969},{"dimgrey",0x696969},{"whitesmoke",0xF5F5F5},{"gainsboro",0xDCDCDC},{"snow",0xFFFAFA},
    {"honeydew",0xF0FFF0},{"mintcream",0xF5FFFA},{"aliceblue",0xF0F8FF},{"ghostwhite",0xF8F8FF},{"steelblue",0x4682B4},
    {"royalblue",0x4169E1},{"skyblue",0x87CEEB},{"dodgerblue",0x1E90FF},{"cornflowerblue",0x6495ED},{"slategray",0x708090},
    {"slategrey",0x708090},{"midnightblue",0x191970},{"seagreen",0x2E8B57},{"limegreen",0x32CD32},{"darkorange",0xFF8C00},
    {"tomato",0xFF6347},{"orangered",0xFF4500},{"hotpink",0xFF69B4},{"deeppink",0xFF1493},{"goldenrod",0xDAA520},
    {"sienna",0xA0522D},{"peru",0xCD853F},{"wheat",0xF5DEB3},{"linen",0xFAF0E6},{"oldlace",0xFDF5E6},{"seashell",0xFFF5EE},
    {"lightcyan",0xE0FFFF},{"lightsalmon",0xFFA07A},{"rebeccapurple",0x663399},{"darkslategray",0x2F4F4F},
    {"darkslategrey",0x2F4F4F},{"cadetblue",0x5F9EA0},{"mediumseagreen",0x3CB371},{"darkviolet",0x9400D3},
    {"mediumpurple",0x9370DB},{"lightsteelblue",0xB0C4DE},{"palegreen",0x98FB98},{"yellowgreen",0x9ACD32},
    {"darkcyan",0x008B8B},{"lightgoldenrodyellow",0xFAFAD2},{"navajowhite",0xFFDEAD},{"bisque",0xFFE4C4},
    {"mistyrose",0xFFE4E1},{"lemonchiffon",0xFFFACD},{"cornsilk",0xFFF8DC},{"antiquewhite",0xFAEBD7},
};

static inline int css_hexv(u8 c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static inline u32 css_blend_white(u32 rgb, int a255) {          /* alpha over a white page */
    if (a255 >= 255) return rgb;
    int r = (int)((rgb >> 16) & 255), g = (int)((rgb >> 8) & 255), b = (int)(rgb & 255);
    r = (r * a255 + 255 * (255 - a255)) / 255; g = (g * a255 + 255 * (255 - a255)) / 255; b = (b * a255 + 255 * (255 - a255)) / 255;
    return (u32)((r << 16) | (g << 8) | b);
}
static inline int css_clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static inline int css_hsl_comp(int t, int m1, int m2) {          /* t in 0..360*, m in 0..1000 */
    if (t < 0) t += 360; if (t >= 360) t -= 360;
    if (t < 60) return m1 + (m2 - m1) * t / 60;
    if (t < 180) return m2;
    if (t < 240) return m1 + (m2 - m1) * (240 - t) / 60;
    return m1;
}

/* returns 1 and fills *out on success */
static inline int css_parse_color(const u8 *s, u32 n, u32 *out) {
    while (n && css_is_ws(s[0])) { s++; n--; }
    while (n && css_is_ws(s[n - 1])) n--;
    if (n == 0) return 0;
    if (s[0] == '#') {
        int d[8]; u32 k = n - 1;
        if (k != 3 && k != 4 && k != 6 && k != 8) return 0;
        for (u32 i = 0; i < k; i++) { d[i] = css_hexv(s[1 + i]); if (d[i] < 0) return 0; }
        int r, g, b, a = 255;
        if (k <= 4) { r = d[0] * 17; g = d[1] * 17; b = d[2] * 17; if (k == 4) a = d[3] * 17; }
        else { r = d[0] * 16 + d[1]; g = d[2] * 16 + d[3]; b = d[4] * 16 + d[5]; if (k == 8) a = d[6] * 16 + d[7]; }
        if (a < 8) { *out = CSS_NOCOLOR; return 1; }
        *out = css_blend_white((u32)((r << 16) | (g << 8) | b), a);
        return 1;
    }
    if (css_kw(s, n, "transparent")) { *out = CSS_NOCOLOR; return 1; }
    if (css_kw(s, n, "currentcolor")) { *out = CSS_CURRENT; return 1; }
    if (n > 4 && (css_kw(s, 3, "rgb") || css_kw(s, 3, "hsl")) && (s[3] == '(' || (css_lc(s[3]) == 'a' && s[4] == '('))) {
        int hsl = css_lc(s[0]) == 'h';
        u32 i = (s[3] == '(') ? 4 : 5;
        int v[4] = {0, 0, 0, 100}; int pctf[4] = {0, 0, 0, 0}; int nv = 0;
        while (i < n && nv < 4) {
            while (i < n && (css_is_ws(s[i]) || s[i] == ',' || s[i] == '/')) i++;
            if (i >= n || s[i] == ')') break;
            int num; u32 c = css_num(s + i, n - i, &num);
            if (!c) return 0;
            i += c;
            if (i < n && s[i] == '%') { pctf[nv] = 1; i++; }
            else if (hsl && nv == 0) { while (i < n && dom_is_alpha(s[i])) i++; }          /* deg */
            v[nv++] = num;
        }
        if (nv < 3) return 0;
        int r, g, b;
        if (!hsl) {
            r = pctf[0] ? v[0] * 255 / 10000 : v[0] / 100; g = pctf[1] ? v[1] * 255 / 10000 : v[1] / 100; b = pctf[2] ? v[2] * 255 / 10000 : v[2] / 100;
        } else {
            int h = v[0] / 100; int sat = v[1] / 10; int l = v[2] / 10;       /* 0..1000 */
            if (sat < 0) sat = 0; if (sat > 1000) sat = 1000; if (l < 0) l = 0; if (l > 1000) l = 1000;
            int m2 = l <= 500 ? l * (1000 + sat) / 1000 : l + sat - l * sat / 1000;
            int m1 = 2 * l - m2;
            r = css_hsl_comp(h + 120, m1, m2) * 255 / 1000; g = css_hsl_comp(h, m1, m2) * 255 / 1000; b = css_hsl_comp(h - 120, m1, m2) * 255 / 1000;
        }
        int a = 255;
        if (nv == 4) a = pctf[3] ? v[3] * 255 / 10000 : v[3] * 255 / 100;
        if (a < 8) { *out = CSS_NOCOLOR; return 1; }
        *out = css_blend_white((u32)((css_clamp255(r) << 16) | (css_clamp255(g) << 8) | css_clamp255(b)), css_clamp255(a));
        return 1;
    }
    for (u32 k = 0; k < sizeof(css_named) / sizeof(css_named[0]); k++)
        if (css_kw(s, n, css_named[k].name)) { *out = css_named[k].rgb; return 1; }
    return 0;
}

/* ---------- tokens inside a declaration value ---------- */
static inline int css_tok(const u8 **pp, const u8 *e, const u8 **ts, u32 *tn) {
    const u8 *p = *pp;
    while (p < e && css_is_ws(*p)) p++;
    if (p >= e) { *pp = p; return 0; }
    const u8 *st = p;
    if (*p == ',' || *p == '/') { p++; }
    else if (*p == '"' || *p == '\'') { u8 q = *p++; while (p < e && *p != q) p++; if (p < e) p++; }
    else {
        while (p < e && !css_is_ws(*p) && *p != ',' && *p != '/') {
            if (*p == '(') { int d = 0; while (p < e) { if (*p == '(') d++; else if (*p == ')') { d--; if (d == 0) { p++; break; } } p++; } }
            else p++;
        }
    }
    *ts = st; *tn = (u32)(p - st); *pp = p;
    return 1;
}

/* ---------- lengths with units ---------- */
/* Parses one token as a length/percentage/number-with-unit into *out (device px). `fpx` is the font size that
 * `em` refers to. `unitless_ok`: a bare number is accepted as px (0 always is). Returns 1 on success. */
static inline int css_calc(const u8 *s, u32 n, css_len_t *out, int fpx);

static inline int css_parse_len(const u8 *s, u32 n, css_len_t *out, int fpx, int unitless_ok) {
    if (n == 0) return 0;
    if (css_kw(s, n, "auto") || css_kw(s, n, "none") || css_kw(s, n, "normal") || css_kw(s, n, "initial") || css_kw(s, n, "inherit") || css_kw(s, n, "unset")) {
        *out = css_len_auto(); return 1;
    }
    if (n > 5 && css_kw(s, 5, "calc(")) return css_calc(s + 5, n - 5, out, fpx);
    if (n > 4 && (css_kw(s, 4, "min(") || css_kw(s, 4, "max("))) return css_calc(s + 4, n - 4, out, fpx);
    if (n > 6 && css_kw(s, 6, "clamp(")) return css_calc(s + 6, n - 6, out, fpx);
    int v; u32 c = css_num(s, n, &v);
    if (!c) return 0;
    const u8 *u = s + c; u32 un = n - c;
    int px100;
    if (un == 0) { if (v != 0 && !unitless_ok) return 0; px100 = v; }
    else if (css_kw(u, un, "px")) px100 = v;
    else if (css_kw(u, un, "em")) px100 = v * fpx;
    else if (css_kw(u, un, "rem")) px100 = v * CSS_BASE_FONT_PX;
    else if (css_kw(u, un, "ex")) px100 = v * fpx / 2;
    else if (css_kw(u, un, "ch")) px100 = v * fpx * 6 / 10;
    else if (css_kw(u, un, "pt")) px100 = v * 4 / 3;
    else if (css_kw(u, un, "pc")) px100 = v * 16;
    else if (css_kw(u, un, "in")) px100 = v * 96;
    else if (css_kw(u, un, "cm")) px100 = v * 3780 / 100;
    else if (css_kw(u, un, "mm")) px100 = v * 378 / 100;
    else if (css_kw(u, un, "q")) px100 = v * 95 / 100;
    else if (css_kw(u, un, "%")) { out->px = 0; out->pct = (short)(v / 10 > 30000 ? 30000 : v / 10 < -30000 ? -30000 : v / 10); return 1; }
    else if (css_kw(u, un, "vw")) { out->px = (short)(css_viewport_w * v / 10000); out->pct = 0; return 1; }
    else if (css_kw(u, un, "vh")) { out->px = (short)(css_viewport_h * v / 10000); out->pct = 0; return 1; }
    else if (css_kw(u, un, "vmin")) { int m = css_viewport_w < css_viewport_h ? css_viewport_w : css_viewport_h; out->px = (short)(m * v / 10000); out->pct = 0; return 1; }
    else if (css_kw(u, un, "vmax")) { int m = css_viewport_w > css_viewport_h ? css_viewport_w : css_viewport_h; out->px = (short)(m * v / 10000); out->pct = 0; return 1; }
    else return 0;
    /* CSS px (x100) -> device px, rounded to nearest */
    int neg = px100 < 0; if (neg) px100 = -px100;
    int dev = (px100 * CSS_ZOOM_NUM + (CSS_ZOOM_DEN * 100) / 2) / (CSS_ZOOM_DEN * 100);
    out->px = (short)(neg ? -dev : dev); out->pct = 0;
    return 1;
}

/* calc()/min()/max()/clamp(): enough for `calc(100% - 2em)`, `min(100%, 600px)`, `clamp(10px, 5vw, 40px)`.
 * `s` points just after the opening '('. */
static inline int css_calc(const u8 *s, u32 n, css_len_t *out, int fpx) {
    /* find the matching close paren */
    u32 depth = 1, e = 0;
    while (e < n && depth) { if (s[e] == '(') depth++; else if (s[e] == ')') { depth--; if (!depth) break; } e++; }
    n = e;
    /* which function was it? look back one char: the caller passed us the text after "calc(" etc, so we sniff
     * by checking for top-level commas (min/max/clamp) vs operators (calc) */
    int has_comma = 0; depth = 0;
    for (u32 i = 0; i < n; i++) { if (s[i] == '(') depth++; else if (s[i] == ')') depth--; else if (s[i] == ',' && depth == 0) has_comma = 1; }
    if (has_comma) {
        css_len_t best = css_len_auto(); int nargs = 0; css_len_t args[3];
        u32 i = 0;
        while (i < n && nargs < 3) {
            while (i < n && css_is_ws(s[i])) i++;
            u32 st = i; depth = 0;
            while (i < n && !(s[i] == ',' && depth == 0)) { if (s[i] == '(') depth++; else if (s[i] == ')') depth--; i++; }
            u32 en = i; while (en > st && css_is_ws(s[en - 1])) en--;
            if (!css_parse_len(s + st, en - st, &args[nargs], fpx, 1)) return 0;
            nargs++; if (i < n) i++;
        }
        if (nargs == 3) { *out = args[1]; return 1; }                  /* clamp(min, preferred, max): the preferred value */
        if (nargs == 2) {                                              /* min()/max(): prefer the pure-px operand (a cap) */
            if (args[0].pct == 0 && args[1].pct != 0) best = args[0];
            else if (args[1].pct == 0 && args[0].pct != 0) best = args[1];
            else best = args[0].px < args[1].px ? args[0] : args[1];
            *out = best; return 1;
        }
        *out = args[0]; return nargs >= 1;
    }
    css_len_t acc = css_len_px(0);
    int sign = 1; u32 i = 0; int first = 1;
    while (i < n) {
        while (i < n && css_is_ws(s[i])) i++;
        if (i >= n) break;
        if (!first) {
            if (s[i] == '+') sign = 1; else if (s[i] == '-') sign = -1; else return 0;
            i++;
            while (i < n && css_is_ws(s[i])) i++;
        }
        u32 st = i; depth = 0;
        while (i < n && !(depth == 0 && css_is_ws(s[i]))) { if (s[i] == '(') depth++; else if (s[i] == ')') depth--; i++; }
        css_len_t t;
        if (!css_parse_len(s + st, i - st, &t, fpx, 1)) return 0;
        acc.px = (short)(acc.px + sign * t.px);
        acc.pct = (short)(acc.pct + sign * (t.pct == CSS_AUTO ? 0 : t.pct));
        first = 0; sign = 1;
    }
    *out = acc; return 1;
}

/* ---------- property table ---------- */
#define CSS_PROP_LIST(X) \
 X(DISPLAY,"display") X(COLOR,"color") X(BACKGROUND,"background") X(BACKGROUND_COLOR,"background-color") \
 X(FONT,"font") X(FONT_SIZE,"font-size") X(FONT_WEIGHT,"font-weight") X(FONT_STYLE,"font-style") X(LINE_HEIGHT,"line-height") \
 X(TEXT_ALIGN,"text-align") X(TEXT_DECORATION,"text-decoration") X(TEXT_DECORATION_LINE,"text-decoration-line") \
 X(TEXT_TRANSFORM,"text-transform") X(TEXT_INDENT,"text-indent") X(WHITE_SPACE,"white-space") X(VISIBILITY,"visibility") \
 X(VERTICAL_ALIGN,"vertical-align") \
 X(MARGIN,"margin") X(MARGIN_TOP,"margin-top") X(MARGIN_RIGHT,"margin-right") X(MARGIN_BOTTOM,"margin-bottom") X(MARGIN_LEFT,"margin-left") \
 X(PADDING,"padding") X(PADDING_TOP,"padding-top") X(PADDING_RIGHT,"padding-right") X(PADDING_BOTTOM,"padding-bottom") X(PADDING_LEFT,"padding-left") \
 X(BORDER,"border") X(BORDER_TOP,"border-top") X(BORDER_RIGHT,"border-right") X(BORDER_BOTTOM,"border-bottom") X(BORDER_LEFT,"border-left") \
 X(BORDER_WIDTH,"border-width") X(BORDER_STYLE,"border-style") X(BORDER_COLOR,"border-color") \
 X(BORDER_TOP_WIDTH,"border-top-width") X(BORDER_RIGHT_WIDTH,"border-right-width") X(BORDER_BOTTOM_WIDTH,"border-bottom-width") X(BORDER_LEFT_WIDTH,"border-left-width") \
 X(BORDER_TOP_STYLE,"border-top-style") X(BORDER_RIGHT_STYLE,"border-right-style") X(BORDER_BOTTOM_STYLE,"border-bottom-style") X(BORDER_LEFT_STYLE,"border-left-style") \
 X(BORDER_TOP_COLOR,"border-top-color") X(BORDER_RIGHT_COLOR,"border-right-color") X(BORDER_BOTTOM_COLOR,"border-bottom-color") X(BORDER_LEFT_COLOR,"border-left-color") \
 X(WIDTH,"width") X(HEIGHT,"height") X(MIN_WIDTH,"min-width") X(MAX_WIDTH,"max-width") X(MIN_HEIGHT,"min-height") X(MAX_HEIGHT,"max-height") \
 X(FLOAT,"float") X(CLEAR,"clear") X(POSITION,"position") X(OVERFLOW,"overflow") X(OVERFLOW_X,"overflow-x") X(OVERFLOW_Y,"overflow-y") \
 X(BOX_SIZING,"box-sizing") X(OPACITY,"opacity") X(LIST_STYLE,"list-style") X(LIST_STYLE_TYPE,"list-style-type") \
 X(BORDER_COLLAPSE,"border-collapse") X(BORDER_SPACING,"border-spacing") \
 X(FLEX,"flex") X(FLEX_DIRECTION,"flex-direction") X(FLEX_WRAP,"flex-wrap") X(FLEX_FLOW,"flex-flow") X(FLEX_GROW,"flex-grow") \
 X(FLEX_SHRINK,"flex-shrink") X(FLEX_BASIS,"flex-basis") X(JUSTIFY_CONTENT,"justify-content") X(ALIGN_ITEMS,"align-items") \
 X(ALIGN_SELF,"align-self") X(GAP,"gap") X(ROW_GAP,"row-gap") X(COLUMN_GAP,"column-gap") X(GRID_GAP,"grid-gap") X(ORDER,"order") \
 X(GRID_TEMPLATE_COLUMNS,"grid-template-columns") X(TOP,"top") X(LEFT,"left") X(RIGHT,"right") X(BOTTOM,"bottom") \
 X(CLIP,"clip") X(CLIP_PATH,"clip-path")

enum {
    CP_NONE = 0,
#define X(id, nm) CP_##id,
    CSS_PROP_LIST(X)
#undef X
    CP__COUNT
};
static const char *const css_prop_names[CP__COUNT] = {
    0,
#define X(id, nm) nm,
    CSS_PROP_LIST(X)
#undef X
};
static inline u32 css_prop_id(const u8 *s, u32 n) {
    if (n < 3 || n > 22) return CP_NONE;
    for (u32 i = 1; i < CP__COUNT; i++) {
        const char *nm = css_prop_names[i];
        u32 k = 0;
        while (k < n && nm[k] && css_lc(s[k]) == (u8)nm[k]) k++;
        if (k == n && nm[k] == 0) return i;
    }
    return CP_NONE;
}

/* ============================================================
 * selectors
 * ============================================================ */
static inline void css_reset(void) {
    css_rule_count = css_comp_count = css_decl_count = css_order = 0;
    css_var_count = 0;
    for (u32 i = 0; i < TG__COUNT; i++) css_by_tag[i] = CSS_NIL;
    for (u32 i = 0; i < CSS_BUCKET_N; i++) { css_by_cls[i] = CSS_NIL; css_by_id[i] = CSS_NIL; }
    css_universal = CSS_NIL;
}

static inline u32 css_ident_end(const u8 *s, u32 i, u32 n) { while (i < n && css_is_ident(s[i])) i++; return i; }

/* an+b from "odd", "even", "3", "2n+1", "-n+3", "n" */
static inline int css_parse_nth(const u8 *s, u32 n, int *a, int *b) {
    while (n && css_is_ws(s[0])) { s++; n--; }
    while (n && css_is_ws(s[n - 1])) n--;
    if (css_kw(s, n, "odd")) { *a = 2; *b = 1; return 1; }
    if (css_kw(s, n, "even")) { *a = 2; *b = 0; return 1; }
    u32 npos = n;
    for (u32 i = 0; i < n; i++) if (css_lc(s[i]) == 'n') { npos = i; break; }
    if (npos == n) { int v; if (!css_num(s, n, &v)) return 0; *a = 0; *b = v / 100; return 1; }
    int av = 100;
    if (npos > 0) {
        if (npos == 1 && s[0] == '-') av = -100; else if (npos == 1 && s[0] == '+') av = 100;
        else if (!css_num(s, npos, &av)) return 0;
    }
    *a = av / 100; *b = 0;
    u32 i = npos + 1;
    while (i < n && css_is_ws(s[i])) i++;
    if (i < n) {
        int sign = 1;
        if (s[i] == '+') i++; else if (s[i] == '-') { sign = -1; i++; } else return 0;
        while (i < n && css_is_ws(s[i])) i++;
        int bv; if (!css_num(s + i, n - i, &bv)) return 0;
        *b = sign * (bv / 100);
    }
    return 1;
}

/* Parses ONE complex selector (no commas) into compounds at the end of RD_CSS_COMPS. Returns the number of
 * compounds, or 0 if it uses something unsupported (the whole rule is then dropped). */
static inline int css_parse_selector(const u8 *s, u32 n, u32 *spec_out) {
    u32 first = css_comp_count, i = 0; int ncomp = 0; u8 comb = 0;
    u32 ids = 0, cls = 0, tags = 0;
    while (i < n && css_is_ws(s[i])) i++;
    while (n && css_is_ws(s[n - 1])) n--;
    while (i < n) {
        if (css_comp_count >= RD_CSS_COMP_MAX) { css_comp_count = first; return 0; }
        css_comp_t *c = &RD_CSS_COMPS[css_comp_count];
        u8 *z = (u8 *)c; for (u32 k = 0; k < sizeof(*c); k++) z[k] = 0;
        c->comb = comb;
        int any = 0;
        for (;;) {
            if (i >= n) break;
            u8 ch = s[i];
            if (ch == '*') { i++; any = 1; continue; }
            if (css_is_ident(ch) && !css_is_digit(ch) && !any) {
                u32 e = css_ident_end(s, i, n);
                u8 nm[16]; u32 ln = e - i; if (ln > 15) ln = 15;
                for (u32 k = 0; k < ln; k++) nm[k] = css_lc(s[i + k]);
                u32 t = dom_tag_id(nm, ln);
                if (t == TG_UNKNOWN) { css_comp_count = first; return 0; }
                c->tag = (u16)t; tags++; i = e; any = 1; continue;
            }
            if (ch == '.') {
                u32 e = css_ident_end(s, i + 1, n);
                if (e == i + 1) { css_comp_count = first; return 0; }
                if (c->ncls < 2) c->cls[c->ncls++] = css_hash(s + i + 1, e - i - 1);
                cls++; i = e; any = 1; continue;
            }
            if (ch == '#') {
                u32 e = css_ident_end(s, i + 1, n);
                if (e == i + 1) { css_comp_count = first; return 0; }
                c->id_hash = css_hash(s + i + 1, e - i - 1); ids++; i = e; any = 1; continue;
            }
            if (ch == '[') {
                u32 e = i + 1, nst = e;
                while (e < n && s[e] != ']') { if (s[e] == '"' || s[e] == '\'') { u8 q = s[e++]; while (e < n && s[e] != q) e++; } e++; }
                if (e >= n) { css_comp_count = first; return 0; }
                u32 j = nst; while (j < e && css_is_ws(s[j])) j++;
                u32 ae = j; while (ae < e && css_is_ident(s[ae])) ae++;
                u8 nm[16]; u32 ln = ae - j; if (ln > 15) ln = 15;
                for (u32 k = 0; k < ln; k++) nm[k] = css_lc(s[j + k]);
                u32 aid = dom_attr_id(nm, ln);
                if (aid == AT_NONE) { css_comp_count = first; return 0; }
                c->attr_id = (u8)aid;
                while (ae < e && css_is_ws(s[ae])) ae++;
                if (ae >= e) c->attr_op = AOP_EXISTS;
                else {
                    u8 op = s[ae];
                    if (op == '=') { c->attr_op = AOP_EQ; ae++; }
                    else if (ae + 1 < e && s[ae + 1] == '=') {
                        c->attr_op = op == '~' ? AOP_WORD : op == '|' ? AOP_DASH : op == '^' ? AOP_PREFIX : op == '$' ? AOP_SUFFIX : op == '*' ? AOP_SUBSTR : 0;
                        ae += 2;
                    }
                    if (!c->attr_op) { css_comp_count = first; return 0; }
                    while (ae < e && css_is_ws(s[ae])) ae++;
                    u32 vs = ae, ve;
                    if (ae < e && (s[ae] == '"' || s[ae] == '\'')) { u8 q = s[ae++]; vs = ae; while (ae < e && s[ae] != q) ae++; ve = ae; if (ae < e) ae++; }
                    else { while (ae < e && !css_is_ws(s[ae])) ae++; ve = ae; }
                    while (ae < e && css_is_ws(s[ae])) ae++;
                    if (ae < e && (s[ae] == 'i' || s[ae] == 'I')) c->attr_icase = 1;
                    c->attr_voff = (u32)(s + vs - RD_POOL); c->attr_vlen = (u16)(ve - vs);
                }
                cls++; i = e + 1; any = 1; continue;
            }
            if (ch == ':') {
                if (i + 1 < n && s[i + 1] == ':') { css_comp_count = first; return 0; }          /* ::before, ::after... */
                u32 e = css_ident_end(s, i + 1, n);
                u32 ln = e - i - 1;
                const u8 *nm = s + i + 1;
                u32 as = 0, ae2 = 0; int has_arg = 0;
                if (e < n && s[e] == '(') {
                    u32 d = 1; as = e + 1; u32 k = as;
                    while (k < n && d) { if (s[k] == '(') d++; else if (s[k] == ')') d--; k++; }
                    if (d) { css_comp_count = first; return 0; }
                    ae2 = k - 1; has_arg = 1; e = k;
                }
                if (css_kw(nm, ln, "first-child") || css_kw(nm, ln, "first-of-type")) c->pseudo |= PS_FIRST;
                else if (css_kw(nm, ln, "last-child") || css_kw(nm, ln, "last-of-type")) c->pseudo |= PS_LAST;
                else if (css_kw(nm, ln, "only-child") || css_kw(nm, ln, "only-of-type")) c->pseudo |= PS_ONLY;
                else if (css_kw(nm, ln, "root")) c->pseudo |= PS_ROOT;
                else if (css_kw(nm, ln, "empty")) c->pseudo |= PS_EMPTY;
                else if (css_kw(nm, ln, "link") || css_kw(nm, ln, "any-link")) c->pseudo |= PS_LINK;
                else if (css_kw(nm, ln, "checked")) c->pseudo |= PS_CHECKED;
                else if (css_kw(nm, ln, "disabled")) c->pseudo |= PS_DISABLED;
                else if (has_arg && (css_kw(nm, ln, "nth-child") || css_kw(nm, ln, "nth-of-type"))) {
                    int a, b;
                    if (!css_parse_nth(s + as, ae2 - as, &a, &b) || a > 100 || a < -100 || b > 100 || b < -100) { css_comp_count = first; return 0; }
                    c->pseudo |= PS_NTH; c->nth_a = (signed char)a; c->nth_b = (signed char)b;
                } else if (has_arg && css_kw(nm, ln, "not")) {
                    const u8 *a = s + as; u32 al = ae2 - as;
                    while (al && css_is_ws(a[0])) { a++; al--; }
                    while (al && css_is_ws(a[al - 1])) al--;
                    if (al < 2) { css_comp_count = first; return 0; }
                    if (a[0] == '.') { c->neg_kind = 2; c->neg_val = css_hash(a + 1, al - 1); }
                    else if (a[0] == '#') { c->neg_kind = 3; c->neg_val = css_hash(a + 1, al - 1); }
                    else if (css_is_ident(a[0])) {
                        u8 tn[16]; u32 tl = al > 15 ? 15 : al;
                        for (u32 k = 0; k < tl; k++) tn[k] = css_lc(a[k]);
                        u32 t = dom_tag_id(tn, tl);
                        if (t == TG_UNKNOWN) { css_comp_count = first; return 0; }
                        c->neg_kind = 1; c->neg_val = t;
                    } else { css_comp_count = first; return 0; }
                    c->pseudo |= PS_NOT;
                } else { css_comp_count = first; return 0; }                         /* :hover :focus :is() ... never match / unsupported */
                cls++; i = e; any = 1; continue;
            }
            break;
        }
        if (!any) { css_comp_count = first; return 0; }
        css_comp_count++; ncomp++;
        /* what follows: a combinator, or the end */
        int saw_ws = 0;
        while (i < n && css_is_ws(s[i])) { i++; saw_ws = 1; }
        if (i >= n) break;
        if (s[i] == '>' || s[i] == '+' || s[i] == '~') { comb = s[i]; i++; while (i < n && css_is_ws(s[i])) i++; }
        else if (saw_ws) comb = ' ';
        else { css_comp_count = first; return 0; }
    }
    if (ncomp == 0) { css_comp_count = first; return 0; }
    *spec_out = (ids > 255 ? 255u : ids) << 16 | (cls > 255 ? 255u : cls) << 8 | (tags > 255 ? 255u : tags);
    return ncomp;
}

static inline void css_bucket_add(u32 ri) {
    css_rule_t *r = &RD_CSS_RULES[ri];
    css_comp_t *last = &RD_CSS_COMPS[r->comp_first + r->ncomp - 1];
    u16 *head;
    if (last->id_hash) { r->key_kind = 3; r->key = last->id_hash; head = &css_by_id[last->id_hash & (CSS_BUCKET_N - 1)]; }
    else if (last->ncls) { r->key_kind = 2; r->key = last->cls[0]; head = &css_by_cls[last->cls[0] & (CSS_BUCKET_N - 1)]; }
    else if (last->tag) { r->key_kind = 1; r->key = last->tag; head = &css_by_tag[last->tag]; }
    else { r->key_kind = 0; r->key = 0; head = &css_universal; }
    r->next = *head; *head = (u16)ri;
}

/* ============================================================
 * declarations & sheets
 * ============================================================ */
/* Walks `name: value [!important];` items in base[*pi..end). Returns 0 when there are no more. */
static inline int css_next_decl(const u8 *base, u32 *pi, u32 end, u32 *ns, u32 *nn, u32 *vs, u32 *vn, int *imp) {
    u32 i = *pi;
    for (;;) {
        while (i < end && (css_is_ws(base[i]) || base[i] == ';')) i++;
        if (i >= end) { *pi = i; return 0; }
        u32 ds = i, colon = end; int depth = 0; u8 q = 0; u32 stop = end;
        for (u32 k = i; k < end; k++) {
            u8 c = base[k];
            if (q) { if (c == q) q = 0; continue; }
            if (c == '"' || c == '\'') q = c;
            else if (c == '(') depth++;
            else if (c == ')') { if (depth) depth--; }
            else if (c == ':' && colon == end && depth == 0) colon = k;
            else if (c == ';' && depth == 0) { stop = k; break; }
        }
        i = stop;
        if (colon >= stop) continue;
        u32 a = ds, b = colon;
        while (b > a && css_is_ws(base[b - 1])) b--;
        u32 v0 = colon + 1, v1 = stop;
        while (v0 < v1 && css_is_ws(base[v0])) v0++;
        while (v1 > v0 && css_is_ws(base[v1 - 1])) v1--;
        *imp = 0;
        if (v1 - v0 >= 10) {                                  /* strip a trailing !important */
            u32 k = v1;
            while (k > v0 && dom_is_alpha(base[k - 1])) k--;
            if (k > v0 + 1 && base[k - 1] == '!' && css_kw(base + k, v1 - k, "important")) {
                *imp = 1; v1 = k - 1;
                while (v1 > v0 && css_is_ws(base[v1 - 1])) v1--;
            }
        }
        *ns = a; *nn = b - a; *vs = v0; *vn = v1 - v0; *pi = i;
        return 1;
    }
}

static inline u32 css_match_brace(const u8 *b, u32 open, u32 end) {
    u32 d = 0; u8 q = 0;
    for (u32 i = open; i < end; i++) {
        u8 c = b[i];
        if (q) { if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') q = c;
        else if (c == '{') d++;
        else if (c == '}') { if (--d == 0) return i; }
    }
    return end;
}

static inline int css_is_rootish(u32 first, int ncomp) {
    if (ncomp != 1) return 0;
    css_comp_t *c = &RD_CSS_COMPS[first];
    if (c->pseudo & PS_ROOT) return 1;
    if (c->tag == TG_HTML || c->tag == TG_BODY) return !c->ncls && !c->id_hash;
    return c->tag == 0 && !c->ncls && !c->id_hash && !c->pseudo && !c->attr_id;
}

static inline void css_add_rule(u8 *b, u32 ss, u32 se, u32 ds, u32 de, int origin) {
    u32 r0 = css_rule_count; int rootish = 0;
    u32 i = ss;
    while (i < se) {
        u32 st = i; int depth = 0; u8 q = 0;
        while (i < se) {
            u8 c = b[i];
            if (q) { if (c == q) q = 0; }
            else if (c == '"' || c == '\'') q = c;
            else if (c == '(' || c == '[') depth++;
            else if (c == ')' || c == ']') { if (depth) depth--; }
            else if (c == ',' && depth == 0) break;
            i++;
        }
        u32 en = i; if (i < se) i++;
        u32 spec;
        u32 cf = css_comp_count;
        int nc = css_parse_selector(b + st, en - st, &spec);
        if (nc > 0 && css_rule_count < RD_CSS_RULE_MAX) {
            css_rule_t *r = &RD_CSS_RULES[css_rule_count];
            r->comp_first = cf; r->ncomp = (u8)nc; r->spec = spec | ((u32)origin << 28);
            r->order = css_order++; r->ndecl = 0; r->decl_first = 0; r->next = CSS_NIL;
            if (css_is_rootish(cf, nc)) rootish = 1;
            css_rule_count++;
        } else if (nc > 0) css_comp_count = cf;
    }
    if (css_rule_count == r0) return;
    u32 d0 = css_decl_count, bo = (u32)(b - RD_POOL);       /* value offsets are stored pool-absolute */
    u32 p = ds, ns, nn, vs, vn; int imp;
    while (css_next_decl(b, &p, de, &ns, &nn, &vs, &vn, &imp)) {
        if (nn > 2 && b[ns] == '-' && b[ns + 1] == '-') {                 /* custom property */
            if (rootish) css_var_set(css_hash(b + ns, nn), bo + vs, vn);
            continue;
        }
        u32 pid = css_prop_id(b + ns, nn);
        if (pid == CP_NONE || css_decl_count >= RD_CSS_DECL_MAX) continue;
        css_decl_t *d = &RD_CSS_DECLS[css_decl_count++];
        d->prop = (u16)pid; d->imp = (u8)imp; d->pad = 0; d->voff = bo + vs; d->vlen = vn;
    }
    u32 nd = css_decl_count - d0;
    for (u32 r = r0; r < css_rule_count; r++) {
        RD_CSS_RULES[r].decl_first = d0; RD_CSS_RULES[r].ndecl = (u16)(nd > 65535 ? 65535 : nd);
        css_bucket_add(r);
    }
}

/* ---- @media ---- */
static inline int css_media_feature(const u8 *s, u32 n) {         /* s = inside the parentheses */
    while (n && css_is_ws(s[0])) { s++; n--; }
    while (n && css_is_ws(s[n - 1])) n--;
    int vw = css_viewport_w * CSS_ZOOM_DEN / CSS_ZOOM_NUM, vh = css_viewport_h * CSS_ZOOM_DEN / CSS_ZOOM_NUM;
    /* range syntax: (width >= 600px) */
    for (u32 i = 0; i + 1 < n; i++) {
        if (s[i] == '<' || s[i] == '>') {
            int lt = s[i] == '<', eq = (s[i + 1] == '='); u32 vs = i + 1 + (eq ? 1 : 0);
            u32 e = i; while (e && css_is_ws(s[e - 1])) e--;
            int dim = css_kw(s, e, "width") ? vw : css_kw(s, e, "height") ? vh : -1;
            if (dim < 0) return 0;
            while (vs < n && css_is_ws(s[vs])) vs++;
            int v; u32 c = css_num(s + vs, n - vs, &v); if (!c) return 0;
            int px = v / 100; if (css_kw(s + vs + c, n - vs - c, "em") || css_kw(s + vs + c, n - vs - c, "rem")) px = v * 16 / 100;
            return lt ? (eq ? dim <= px : dim < px) : (eq ? dim >= px : dim > px);
        }
    }
    u32 colon = n; for (u32 i = 0; i < n; i++) if (s[i] == ':') { colon = i; break; }
    u32 fe = colon; while (fe > 0 && css_is_ws(s[fe - 1])) fe--;
    const u8 *val = s + (colon < n ? colon + 1 : n); u32 vl = colon < n ? n - colon - 1 : 0;
    while (vl && css_is_ws(val[0])) { val++; vl--; }
    if (css_kw(s, fe, "color") && colon == n) return 1;
    if (colon == n) return 0;
    int v = 0; u32 c = css_num(val, vl, &v);
    int px = 0;
    if (c) { px = v / 100; if (css_kw(val + c, vl - c, "em") || css_kw(val + c, vl - c, "rem")) px = v * 16 / 100; }
    if (css_kw(s, fe, "min-width")) return c && vw >= px;
    if (css_kw(s, fe, "max-width")) return c && vw <= px;
    if (css_kw(s, fe, "width")) return c && vw == px;
    if (css_kw(s, fe, "min-height")) return c && vh >= px;
    if (css_kw(s, fe, "max-height")) return c && vh <= px;
    if (css_kw(s, fe, "orientation")) return css_kw(val, vl, "landscape") ? vw >= vh : vw < vh;
    if (css_kw(s, fe, "prefers-color-scheme")) return css_kw(val, vl, "light");
    if (css_kw(s, fe, "prefers-reduced-motion")) return css_kw(val, vl, "no-preference");
    if (css_kw(s, fe, "hover") || css_kw(s, fe, "any-hover")) return css_kw(val, vl, "hover");
    if (css_kw(s, fe, "pointer") || css_kw(s, fe, "any-pointer")) return css_kw(val, vl, "fine");
    if (css_kw(s, fe, "forced-colors")) return css_kw(val, vl, "none");
    return 0;
}
static inline int css_media_eval(const u8 *s, u32 n) {
    u32 i = 0;
    if (n == 0) return 1;
    while (i <= n) {
        u32 st = i; int depth = 0;
        while (i < n && !(s[i] == ',' && depth == 0)) { if (s[i] == '(') depth++; else if (s[i] == ')') depth--; i++; }
        u32 en = i; i++;
        /* one query */
        u32 k = st; int neg = 0, ok = 1, saw_type = 0;
        while (k < en) {
            while (k < en && css_is_ws(s[k])) k++;
            if (k >= en) break;
            if (s[k] == '(') {
                u32 d = 1, e = k + 1; while (e < en && d) { if (s[e] == '(') d++; else if (s[e] == ')') d--; e++; }
                if (!css_media_feature(s + k + 1, e - k - 2)) ok = 0;
                k = e; continue;
            }
            u32 e = k; while (e < en && !css_is_ws(s[e]) && s[e] != '(') e++;
            if (css_kw(s + k, e - k, "not")) neg = 1;
            else if (css_kw(s + k, e - k, "only") || css_kw(s + k, e - k, "and")) { }
            else if (css_kw(s + k, e - k, "all") || css_kw(s + k, e - k, "screen")) saw_type = 1;
            else { ok = 0; saw_type = 1; }                                        /* print, speech, tv... */
            k = e;
        }
        (void)saw_type;
        if (neg) ok = !ok;
        if (ok) return 1;
        if (i > n) break;
    }
    return 0;
}

static inline void css_parse_range(u8 *b, u32 i, u32 end, int origin, int depth) {
    while (i < end) {
        while (i < end && (css_is_ws(b[i]) || b[i] == '}' || b[i] == ';')) i++;
        if (i >= end) break;
        if (b[i] == '@') {
            u32 ne = css_ident_end(b, i + 1, end);
            u32 j = ne;
            while (j < end && b[j] != '{' && b[j] != ';') j++;
            if (j >= end) break;
            if (b[j] == ';') { i = j + 1; continue; }
            u32 k = css_match_brace(b, j, end);
            u32 nl = ne - i - 1;
            if (depth < 4) {
                if (css_kw(b + i + 1, nl, "media")) { if (css_media_eval(b + ne, j - ne)) css_parse_range(b, j + 1, k, origin, depth + 1); }
                else if (css_kw(b + i + 1, nl, "supports") || css_kw(b + i + 1, nl, "layer") || css_kw(b + i + 1, nl, "document"))
                    css_parse_range(b, j + 1, k, origin, depth + 1);
            }
            i = k + 1; continue;
        }
        u32 j = i; u8 q = 0;
        while (j < end) { u8 c = b[j]; if (q) { if (c == q) q = 0; } else if (c == '"' || c == '\'') q = c; else if (c == '{') break; j++; }
        if (j >= end) break;
        u32 k = css_match_brace(b, j, end);
        css_add_rule(b, i, j, j + 1, k, origin);
        i = k + 1;
    }
}

/* Parses the stylesheet text at RD_POOL[off .. off+len). Comments (and the old <!-- --> hiding trick) are blanked in place. */
static inline void css_parse_sheet(u32 off, u32 len, int origin) {
    u8 *b = RD_POOL + off;
    for (u32 i = 0; i + 1 < len; i++) {
        if (b[i] == '/' && b[i + 1] == '*') {
            u32 j = i + 2; while (j + 1 < len && !(b[j] == '*' && b[j + 1] == '/')) j++;
            u32 e = j + 1 < len ? j + 2 : len;
            for (u32 k = i; k < e; k++) b[k] = ' ';
            i = e - 1;
        } else if (b[i] == '<' && i + 3 < len && b[i + 1] == '!' && b[i + 2] == '-' && b[i + 3] == '-') { b[i] = b[i + 1] = b[i + 2] = b[i + 3] = ' '; }
        else if (b[i] == '-' && i + 2 < len && b[i + 1] == '-' && b[i + 2] == '>') { b[i] = b[i + 1] = b[i + 2] = ' '; }
    }
    css_parse_range(b, 0, len, origin, 0);
}

/* The user-agent sheet: what a browser looks like before a website has an opinion. */
static const char css_ua_sheet[] =
"html,body,address,blockquote,center,dialog,div,figure,figcaption,footer,form,header,hgroup,main,menu,nav,section,article,aside,"
"details,summary,dir,dl,dt,dd,ol,ul,fieldset,legend,optgroup,p,pre,listing,xmp,plaintext,hr,h1,h2,h3,h4,h5,h6,search,marquee{display:block}"
"head,link,meta,script,style,title,base,template,area,datalist,param,source,track,noembed,noframes,rt,rp,option,[hidden],dialog,frameset,frame{display:none}"
"li{display:list-item}"
"table{display:table;border-collapse:separate;border-spacing:2px}caption{display:table-caption;text-align:center}"
"tr{display:table-row}td,th{display:table-cell;padding:1px}thead,tbody,tfoot{display:table-row-group}col,colgroup{display:none}"
"th{font-weight:bold;text-align:center}"
"img,input,select,textarea,button,iframe,canvas,video,audio,object,embed,svg,math,progress,meter{display:inline-block}"
"body{margin:8px}"
"p,dl,pre,listing,xmp,plaintext{margin:1em 0}blockquote,figure{margin:1em 40px}dd{margin-left:40px}"
"ul,ol,dir,menu{margin:1em 0;padding-left:40px}ul ul,ul ol,ol ul,ol ol,ul dir,dir ul{margin:0}"
"ul{list-style-type:disc}ul ul{list-style-type:circle}ul ul ul{list-style-type:square}ol{list-style-type:decimal}"
"h1{font-size:2em;margin:.67em 0}h2{font-size:1.5em;margin:.83em 0}h3{font-size:1.17em;margin:1em 0}"
"h4{margin:1.33em 0}h5{font-size:.83em;margin:1.67em 0}h6{font-size:.67em;margin:2.33em 0}"
"h1,h2,h3,h4,h5,h6,b,strong,th{font-weight:bold}"
"i,em,cite,dfn,var,address{font-style:italic}u,ins{text-decoration:underline}s,strike,del{text-decoration:line-through}"
"a[href]{color:#0000EE;text-decoration:underline}"
"small{font-size:.83em}big{font-size:1.17em}sub{vertical-align:sub;font-size:.83em}sup{vertical-align:super;font-size:.83em}"
"mark{background-color:#FFFF00;color:#000000}center{text-align:center}nobr{white-space:nowrap}"
"pre,listing,xmp,plaintext{white-space:pre}"
"hr{margin:8px auto;border:1px inset #808080;height:0}"
"fieldset{border:2px groove #C0C0C0;padding:.35em .75em .625em;margin:0 2px}legend{padding:0 2px}"
"button,input[type=submit],input[type=button],input[type=reset]{border:2px outset #E0E0E0;background-color:#D4D0C8;padding:1px 6px;text-align:center}"
"input,textarea,select{border:2px inset #808080;background-color:#FFFFFF;padding:1px 2px}"
"input[type=checkbox],input[type=radio],input[type=hidden],input[type=image]{border:0;background-color:transparent;padding:0}"
"input[type=hidden]{display:none}"
"img{border:0}"
"summary{display:block;padding-left:1.6em}details>summary{font-weight:bold}"
"progress,meter{border:1px solid #808080;background-color:#E0E0E0}";

/* ============================================================
 * matching
 * ============================================================ */
static inline u32 css_parent_elem(u32 node) {
    u32 p = RD_NODES[node].parent;
    return (p && RD_NODES[p].kind == RDK_ELEM) ? p : 0;
}
static inline u32 css_prev_elem(u32 node) {
    u32 p = RD_NODES[node].parent, prev = 0;
    if (!p) return 0;
    for (u32 c = RD_NODES[p].first; c && c != node; c = RD_NODES[c].next) if (RD_NODES[c].kind == RDK_ELEM) prev = c;
    return prev;
}
static inline int css_has_class(u32 node, u32 h) {
    u32 len; const u8 *v = dom_attr(node, AT_CLASS, &len);
    if (!v) return 0;
    for (u32 i = 0; i < len; ) {
        while (i < len && css_is_ws(v[i])) i++;
        u32 st = i; while (i < len && !css_is_ws(v[i])) i++;
        if (i > st && css_hash(v + st, i - st) == h) return 1;
    }
    return 0;
}
static inline int css_attr_match(u32 node, const css_comp_t *c) {
    u32 len; const u8 *v = dom_attr(node, c->attr_id, &len);
    if (!v) return 0;
    if (c->attr_op == AOP_EXISTS) return 1;
    const u8 *w = RD_POOL + c->attr_voff; u32 wl = c->attr_vlen;
    int ic = c->attr_icase;
    #define CMP(a, b) (ic ? css_lc(a) == css_lc(b) : (a) == (b))
    switch (c->attr_op) {
        case AOP_EQ: if (len != wl) return 0; for (u32 i = 0; i < wl; i++) if (!CMP(v[i], w[i])) return 0; return 1;
        case AOP_PREFIX: if (wl == 0 || len < wl) return 0; for (u32 i = 0; i < wl; i++) if (!CMP(v[i], w[i])) return 0; return 1;
        case AOP_SUFFIX: if (wl == 0 || len < wl) return 0; for (u32 i = 0; i < wl; i++) if (!CMP(v[len - wl + i], w[i])) return 0; return 1;
        case AOP_SUBSTR: if (wl == 0) return 0;
            for (u32 s = 0; s + wl <= len; s++) { u32 k = 0; while (k < wl && CMP(v[s + k], w[k])) k++; if (k == wl) return 1; }
            return 0;
        case AOP_DASH: if (len < wl) return 0; for (u32 i = 0; i < wl; i++) if (!CMP(v[i], w[i])) return 0; return len == wl || v[wl] == '-';
        case AOP_WORD:
            for (u32 i = 0; i < len; ) {
                while (i < len && css_is_ws(v[i])) i++;
                u32 st = i; while (i < len && !css_is_ws(v[i])) i++;
                if (i - st == wl && wl) { u32 k = 0; while (k < wl && CMP(v[st + k], w[k])) k++; if (k == wl) return 1; }
            }
            return 0;
    }
    #undef CMP
    return 0;
}
static inline int css_comp_ok(u32 node, const css_comp_t *c) {
    rd_node_t *n = &RD_NODES[node];
    if (n->kind != RDK_ELEM) return 0;
    if (c->tag && n->tag != c->tag) return 0;
    if (c->id_hash) {
        u32 len; const u8 *v = dom_attr(node, AT_ID, &len);
        if (!v || css_hash(v, len) != c->id_hash) return 0;
    }
    for (u32 i = 0; i < c->ncls; i++) if (!css_has_class(node, c->cls[i])) return 0;
    if (c->attr_id && !css_attr_match(node, c)) return 0;
    if (c->pseudo) {
        u16 ps = c->pseudo;
        if (ps & PS_ROOT) { if (node != dom_html) return 0; }
        if (ps & PS_EMPTY) { if (n->first) return 0; }
        if (ps & PS_LINK) { if (n->tag != TG_A && n->tag != TG_AREA) return 0; if (!dom_has_attr(node, AT_HREF)) return 0; }
        if (ps & PS_CHECKED) { if (!dom_has_attr(node, AT_CHECKED) && !dom_has_attr(node, AT_SELECTED)) return 0; }
        if (ps & PS_DISABLED) { if (!dom_has_attr(node, AT_DISABLED)) return 0; }
        if (ps & (PS_FIRST | PS_LAST | PS_ONLY | PS_NTH)) {
            u32 p = n->parent; int pos = 0, total = 0, found = 0;
            for (u32 ch = RD_NODES[p].first; ch; ch = RD_NODES[ch].next) {
                if (RD_NODES[ch].kind != RDK_ELEM) continue;
                total++; if (ch == node) { pos = total; found = 1; }
            }
            if (!found) return 0;
            if ((ps & PS_FIRST) && pos != 1) return 0;
            if ((ps & PS_LAST) && pos != total) return 0;
            if ((ps & PS_ONLY) && total != 1) return 0;
            if (ps & PS_NTH) {
                int a = c->nth_a, b = c->nth_b;
                if (a == 0) { if (pos != b) return 0; }
                else { int d = pos - b; if (d % a != 0 || d / a < 0) return 0; }
            }
        }
        if (ps & PS_NOT) {
            int hit = 0;
            if (c->neg_kind == 1) hit = n->tag == c->neg_val;
            else if (c->neg_kind == 2) hit = css_has_class(node, c->neg_val);
            else if (c->neg_kind == 3) { u32 len; const u8 *v = dom_attr(node, AT_ID, &len); hit = v && css_hash(v, len) == c->neg_val; }
            if (hit) return 0;
        }
    }
    return 1;
}
static inline int css_match_from(u32 node, const css_comp_t *comps, int idx) {
    if (!css_comp_ok(node, &comps[idx])) return 0;
    if (idx == 0) return 1;
    switch (comps[idx].comb) {
        case '>': { u32 p = css_parent_elem(node); return p && css_match_from(p, comps, idx - 1); }
        case ' ': for (u32 p = css_parent_elem(node); p; p = css_parent_elem(p)) if (css_match_from(p, comps, idx - 1)) return 1; return 0;
        case '+': { u32 p = css_prev_elem(node); return p && css_match_from(p, comps, idx - 1); }
        case '~': { u32 p = css_prev_elem(node); while (p) { if (css_match_from(p, comps, idx - 1)) return 1; p = css_prev_elem(p); } return 0; }
    }
    return 0;
}


/* ============================================================
 * computed style: initial values, inheritance, applying declarations
 * ============================================================ */
static inline void css_zero(void *p, u32 n) { u8 *b = (u8 *)p; for (u32 i = 0; i < n; i++) b[i] = 0; }

static inline void css_style_init(css_style_t *s) {
    css_zero(s, sizeof(*s));
    s->display = DISP_INLINE; s->color = 0x000000; s->bg = CSS_NOCOLOR;
    s->fpx = CSS_BASE_FONT_PX; s->cell = CSS_MIN_CELL;
    for (int i = 0; i < 4; i++) s->bcolor[i] = CSS_CURRENT;
    s->width = s->height = s->minw = s->minh = s->maxw = s->maxh = s->flex_basis = css_len_auto();
    s->flex_shrink = 10; s->align_items = AI_STRETCH; s->align_self = AI_AUTO;
    s->list_style = LS_DISC;
}
static inline void css_style_inherit(css_style_t *s, const css_style_t *par) {
    css_style_init(s);
    if (!par) return;
    s->color = par->color; s->fpx = par->fpx; s->cell = par->cell; s->flags = par->flags;
    s->text_align = par->text_align; s->white_space = par->white_space; s->visibility = par->visibility;
    s->text_transform = par->text_transform; s->list_style = par->list_style; s->lh = par->lh; s->lh_mode = par->lh_mode;
    s->border_collapse = par->border_collapse; s->spacing_x = par->spacing_x; s->spacing_y = par->spacing_y;
    s->text_indent = par->text_indent; s->hidden_text = par->hidden_text;
}

static inline int css_cell_for(int fpx) {
    int c = (fpx * CSS_ZOOM_NUM + CSS_ZOOM_DEN / 2) / CSS_ZOOM_DEN;
    return c < CSS_MIN_CELL ? CSS_MIN_CELL : c > CSS_MAX_CELL ? CSS_MAX_CELL : c;
}
static inline void css_set_fpx(css_style_t *s, int fpx) {
    if (fpx < 1) fpx = 1; if (fpx > 400) fpx = 400;
    s->fpx = (short)fpx; s->cell = (u8)css_cell_for(fpx);
}
/* font-size value -> CSS px, or -1 */
static inline int css_parse_fontsize(const u8 *v, u32 n, int parfpx) {
    if (css_kw(v, n, "xx-small")) return 9;   if (css_kw(v, n, "x-small")) return 10;  if (css_kw(v, n, "small")) return 13;
    if (css_kw(v, n, "medium")) return 16;    if (css_kw(v, n, "large")) return 18;    if (css_kw(v, n, "x-large")) return 24;
    if (css_kw(v, n, "xx-large")) return 32;  if (css_kw(v, n, "xxx-large")) return 48;
    if (css_kw(v, n, "smaller")) return parfpx * 5 / 6; if (css_kw(v, n, "larger")) return parfpx * 6 / 5;
    int x; u32 c = css_num(v, n, &x);
    if (!c) return -1;
    const u8 *u = v + c; u32 un = n - c;
    if (css_kw(u, un, "px") || (un == 0 && x == 0)) return x / 100;
    if (css_kw(u, un, "em")) return parfpx * x / 100;
    if (css_kw(u, un, "rem")) return CSS_BASE_FONT_PX * x / 100;
    if (css_kw(u, un, "%")) return parfpx * x / 10000;
    if (css_kw(u, un, "pt")) return x * 4 / 300;
    if (css_kw(u, un, "ex")) return parfpx * x / 200;
    return -1;
}

/* one to four lengths -> top,right,bottom,left */
static inline int css_parse_sides(const u8 *v, u32 n, css_len_t out[4], int fpx, int allow_auto) {
    css_len_t t[4]; int k = 0;
    const u8 *p = v, *e = v + n, *ts; u32 tn;
    while (css_tok(&p, e, &ts, &tn)) {
        if (ts[0] == ',' || ts[0] == '/') return 0;
        if (k >= 4 || !css_parse_len(ts, tn, &t[k], fpx, 0)) return 0;
        if (!allow_auto && css_len_is_auto(t[k])) t[k] = css_len_px(0);
        k++;
    }
    if (k == 0) return 0;
    if (k == 1) { out[0] = out[1] = out[2] = out[3] = t[0]; }
    else if (k == 2) { out[0] = out[2] = t[0]; out[1] = out[3] = t[1]; }
    else if (k == 3) { out[0] = t[0]; out[1] = out[3] = t[1]; out[2] = t[2]; }
    else { out[0] = t[0]; out[1] = t[1]; out[2] = t[2]; out[3] = t[3]; }
    return 1;
}

static inline int css_border_style_kw(const u8 *v, u32 n) {
    if (css_kw(v, n, "none") || css_kw(v, n, "hidden")) return BS_NONE;
    if (css_kw(v, n, "solid")) return BS_SOLID;   if (css_kw(v, n, "dashed")) return BS_DASHED;
    if (css_kw(v, n, "dotted")) return BS_DOTTED; if (css_kw(v, n, "double")) return BS_DOUBLE;
    if (css_kw(v, n, "inset")) return BS_INSET;   if (css_kw(v, n, "outset")) return BS_OUTSET;
    if (css_kw(v, n, "groove")) return BS_GROOVE; if (css_kw(v, n, "ridge")) return BS_RIDGE;
    return -1;
}
static inline int css_border_width_tok(const u8 *v, u32 n, int fpx) {
    if (css_kw(v, n, "thin")) return 1;
    if (css_kw(v, n, "medium")) return 2;
    if (css_kw(v, n, "thick")) return 3;
    css_len_t l;
    if (!css_parse_len(v, n, &l, fpx, 0) || l.pct) return -1;
    return l.px < 0 ? 0 : l.px > 40 ? 40 : l.px;
}
static inline void css_set_color(u32 *dst, u32 c, u32 cur) { *dst = (c == CSS_CURRENT) ? cur : c; }

static inline void css_apply_border(css_style_t *s, int mask, const u8 *v, u32 n) {
    int w = -1, st = -1; u32 col = CSS_CURRENT; int got_col = 0;
    const u8 *p = v, *e = v + n, *ts; u32 tn;
    while (css_tok(&p, e, &ts, &tn)) {
        int x;
        if ((x = css_border_style_kw(ts, tn)) >= 0) st = x;
        else if ((x = css_border_width_tok(ts, tn, s->fpx)) >= 0) w = x;
        else { u32 c; if (css_parse_color(ts, tn, &c)) { col = c; got_col = 1; } }
    }
    if (st < 0) st = BS_NONE;
    if (w < 0) w = 2;
    for (int i = 0; i < 4; i++) if (mask & (1 << i)) {
        s->bstyle[i] = (u8)st; s->bw[i] = (u8)(st == BS_NONE ? 0 : w);
        s->bcolor[i] = got_col ? col : CSS_CURRENT;
    }
}

static inline int css_list_style_kw(const u8 *v, u32 n) {
    if (css_kw(v, n, "none")) return LS_NONE;
    if (css_kw(v, n, "disc")) return LS_DISC;       if (css_kw(v, n, "circle")) return LS_CIRCLE;
    if (css_kw(v, n, "square")) return LS_SQUARE;
    if (css_kw(v, n, "decimal") || css_kw(v, n, "decimal-leading-zero")) return LS_DECIMAL;
    if (css_kw(v, n, "lower-alpha") || css_kw(v, n, "lower-latin")) return LS_LOWER_ALPHA;
    if (css_kw(v, n, "upper-alpha") || css_kw(v, n, "upper-latin")) return LS_UPPER_ALPHA;
    if (css_kw(v, n, "lower-roman")) return LS_LOWER_ROMAN; if (css_kw(v, n, "upper-roman")) return LS_UPPER_ROMAN;
    if (css_kw(v, n, "korean-hangul-formal") || css_kw(v, n, "hangul") || css_kw(v, n, "cjk-decimal")) return LS_DECIMAL;
    return -1;
}

static inline int css_flex_align_kw(const u8 *v, u32 n, int allow_auto) {
    if (css_kw(v, n, "stretch") || css_kw(v, n, "normal")) return AI_STRETCH;
    if (css_kw(v, n, "flex-start") || css_kw(v, n, "start") || css_kw(v, n, "baseline") || css_kw(v, n, "self-start")) return AI_START;
    if (css_kw(v, n, "flex-end") || css_kw(v, n, "end") || css_kw(v, n, "self-end")) return AI_END;
    if (css_kw(v, n, "center")) return AI_CENTER;
    if (allow_auto && css_kw(v, n, "auto")) return AI_AUTO;
    return -1;
}

/* a color hiding inside linear-gradient(...) etc: the first color stop stands in for the whole gradient */
static inline int css_gradient_color(const u8 *v, u32 n, u32 *out) {
    u32 i = 0; while (i < n && v[i] != '(') i++;
    i++;
    while (i < n) {
        u32 st = i; int d = 0;
        while (i < n && !(v[i] == ',' && d == 0) && !(v[i] == ')' && d == 0)) { if (v[i] == '(') d++; else if (v[i] == ')') d--; i++; }
        u32 en = i; while (en > st && css_is_ws(v[en - 1])) en--;
        u32 k = st; while (k < en && css_is_ws(v[k])) k++;
        u32 sp = k, dd = 0; while (sp < en && !(css_is_ws(v[sp]) && dd == 0)) { if (v[sp] == '(') dd++; else if (v[sp] == ')') dd--; sp++; }
        if (css_parse_color(v + k, sp - k, out)) return 1;
        if (i < n && v[i] == ')') break;
        i++;
    }
    return 0;
}

static inline void css_apply(css_style_t *s, u32 prop, const u8 *v, u32 n, const css_style_t *par) {
    u8 buf[384];
    for (u32 i = 0; i + 3 < n; i++) {
        if (css_lc(v[i]) == 'v' && css_lc(v[i + 1]) == 'a' && css_lc(v[i + 2]) == 'r' && v[i + 3] == '(') { n = css_expand(v, n, buf, sizeof(buf), 0); v = buf; break; }
    }
    if (n == 0) return;
    #define KW(x) css_kw(v, n, x)
    if (KW("inherit") && par) {
        switch (prop) {
            case CP_COLOR: s->color = par->color; break;
            case CP_FONT_SIZE: s->fpx = par->fpx; s->cell = par->cell; break;
            case CP_FONT_WEIGHT: s->flags = (u8)((s->flags & ~SF_BOLD) | (par->flags & SF_BOLD)); break;
            case CP_FONT_STYLE: s->flags = (u8)((s->flags & ~SF_ITALIC) | (par->flags & SF_ITALIC)); break;
            case CP_TEXT_ALIGN: s->text_align = par->text_align; break;
            case CP_LINE_HEIGHT: s->lh = par->lh; s->lh_mode = par->lh_mode; break;
            case CP_VISIBILITY: s->visibility = par->visibility; break;
            case CP_WHITE_SPACE: s->white_space = par->white_space; break;
            case CP_DISPLAY: s->display = par->display == DISP_NONE ? DISP_INLINE : par->display; break;
            case CP_BACKGROUND: case CP_BACKGROUND_COLOR: s->bg = par->bg; break;
            default: break;
        }
        return;
    }
    const u8 *ts; u32 tn;
    switch (prop) {
        case CP_DISPLAY: {
            const u8 *p = v, *e = v + n;
            if (!css_tok(&p, e, &ts, &tn)) return;
            int d = -1;
            if (css_kw(ts, tn, "none")) d = DISP_NONE;
            else if (css_kw(ts, tn, "block") || css_kw(ts, tn, "flow-root") || css_kw(ts, tn, "-webkit-box")) d = DISP_BLOCK;
            else if (css_kw(ts, tn, "inline") || css_kw(ts, tn, "contents") || css_kw(ts, tn, "ruby")) d = DISP_INLINE;
            else if (css_kw(ts, tn, "inline-block") || css_kw(ts, tn, "inline-grid")) d = DISP_INLINE_BLOCK;
            else if (css_kw(ts, tn, "list-item")) d = DISP_LIST_ITEM;
            else if (css_kw(ts, tn, "table")) d = DISP_TABLE;
            else if (css_kw(ts, tn, "inline-table")) d = DISP_INLINE_TABLE;
            else if (css_kw(ts, tn, "table-row")) d = DISP_TABLE_ROW;
            else if (css_kw(ts, tn, "table-cell")) d = DISP_TABLE_CELL;
            else if (css_kw(ts, tn, "table-row-group") || css_kw(ts, tn, "table-header-group") || css_kw(ts, tn, "table-footer-group")) d = DISP_TABLE_GROUP;
            else if (css_kw(ts, tn, "table-caption")) d = DISP_TABLE_CAPTION;
            else if (css_kw(ts, tn, "table-column") || css_kw(ts, tn, "table-column-group")) d = DISP_NONE;
            else if (css_kw(ts, tn, "flex")) d = DISP_FLEX;
            else if (css_kw(ts, tn, "inline-flex")) d = DISP_INLINE_FLEX;
            else if (css_kw(ts, tn, "grid")) d = DISP_GRID;
            if (d >= 0) s->display = (u8)d;
        } break;
        case CP_COLOR: { u32 c; if (css_parse_color(v, n, &c)) css_set_color(&s->color, c, par ? par->color : 0); } break;
        case CP_BACKGROUND_COLOR: { u32 c; if (css_parse_color(v, n, &c)) css_set_color(&s->bg, c, s->color); } break;
        case CP_BACKGROUND: {
            u32 col = CSS_NOCOLOR; const u8 *p = v, *e = v + n;
            while (css_tok(&p, e, &ts, &tn)) {
                u32 c;
                if (css_parse_color(ts, tn, &c)) col = c;
                else if (tn > 8 && (css_kw(ts, 7, "linear-") || css_kw(ts, 7, "radial-") || css_kw(ts, 9, "repeating")) && css_gradient_color(ts, tn, &c)) col = c;
            }
            css_set_color(&s->bg, col, s->color);
        } break;
        case CP_FONT_SIZE: { int px = css_parse_fontsize(v, n, par ? par->fpx : CSS_BASE_FONT_PX); if (px >= 0) css_set_fpx(s, px); } break;
        case CP_FONT_WEIGHT: {
            int x;
            if (KW("bold") || KW("bolder")) s->flags |= SF_BOLD;
            else if (KW("normal") || KW("lighter")) s->flags &= (u8)~SF_BOLD;
            else if (css_num(v, n, &x)) { if (x >= 60000) s->flags |= SF_BOLD; else s->flags &= (u8)~SF_BOLD; }
        } break;
        case CP_FONT_STYLE:
            if (KW("italic") || KW("oblique")) s->flags |= SF_ITALIC; else if (KW("normal")) s->flags &= (u8)~SF_ITALIC;
            break;
        case CP_FONT: {
            const u8 *p = v, *e = v + n; int seen_size = 0;
            if (KW("inherit")) return;
            s->flags &= (u8)~(SF_BOLD | SF_ITALIC);
            while (!seen_size && css_tok(&p, e, &ts, &tn)) {
                int x;
                if (css_kw(ts, tn, "italic") || css_kw(ts, tn, "oblique")) s->flags |= SF_ITALIC;
                else if (css_kw(ts, tn, "bold") || css_kw(ts, tn, "bolder")) s->flags |= SF_BOLD;
                else if (css_kw(ts, tn, "normal") || css_kw(ts, tn, "small-caps")) { }
                else if (css_num(ts, tn, &x) == tn && x >= 10000) { if (x >= 60000) s->flags |= SF_BOLD; }
                else {
                    int px = css_parse_fontsize(ts, tn, par ? par->fpx : CSS_BASE_FONT_PX);
                    if (px >= 0) {
                        css_set_fpx(s, px); seen_size = 1;
                        const u8 *q = p;
                        if (css_tok(&q, e, &ts, &tn) && ts[0] == '/') {
                            p = q;
                            if (css_tok(&p, e, &ts, &tn)) css_apply(s, CP_LINE_HEIGHT, ts, tn, par);
                        }
                    }
                }
            }
        } break;
        case CP_LINE_HEIGHT: {
            int x; u32 c = css_num(v, n, &x);
            if (KW("normal")) { s->lh_mode = 0; s->lh = 0; }
            else if (c == n) { s->lh_mode = 2; s->lh = (short)(x > 3000 ? 3000 : x < 0 ? 0 : x); }
            else {
                css_len_t l;
                if (css_parse_len(v, n, &l, s->fpx, 0) && !css_len_is_auto(l)) {
                    if (l.pct) { s->lh_mode = 2; s->lh = (short)(l.pct / 10); } else { s->lh_mode = 1; s->lh = l.px; }
                }
            }
        } break;
        case CP_TEXT_ALIGN:
            if (KW("left") || KW("start") || KW("-webkit-left")) s->text_align = TA_LEFT;
            else if (KW("center") || KW("-webkit-center") || KW("-moz-center")) s->text_align = TA_CENTER;
            else if (KW("right") || KW("end") || KW("-webkit-right")) s->text_align = TA_RIGHT;
            else if (KW("justify")) s->text_align = TA_JUSTIFY;
            break;
        case CP_TEXT_DECORATION: case CP_TEXT_DECORATION_LINE: {
            const u8 *p = v, *e = v + n; u8 f = 0; int none = 0;
            while (css_tok(&p, e, &ts, &tn)) {
                if (css_kw(ts, tn, "underline")) f |= SF_UNDER;
                else if (css_kw(ts, tn, "line-through")) f |= SF_STRIKE;
                else if (css_kw(ts, tn, "none")) none = 1;
            }
            if (none) s->flags &= (u8)~(SF_UNDER | SF_STRIKE);
            s->flags |= f;
        } break;
        case CP_TEXT_TRANSFORM:
            s->text_transform = KW("uppercase") ? TT_UPPER : KW("lowercase") ? TT_LOWER : KW("capitalize") ? TT_CAP : TT_NONE;
            break;
        case CP_TEXT_INDENT: { css_len_t l; if (css_parse_len(v, n, &l, s->fpx, 0) && !l.pct) { if (l.px <= -300) s->hidden_text = 1; else s->text_indent = l.px; } } break;
        case CP_WHITE_SPACE:
            s->white_space = KW("nowrap") ? WS_NOWRAP : KW("pre") ? WS_PRE : (KW("pre-wrap") || KW("break-spaces")) ? WS_PRE_WRAP :
                             KW("pre-line") ? WS_PRE_LINE : WS_NORMAL;
            break;
        case CP_VISIBILITY: s->visibility = (KW("hidden") || KW("collapse")) ? 1 : 0; break;
        case CP_OPACITY: { int x; if (css_num(v, n, &x) && x <= 5) s->visibility = 1; } break;
        case CP_VERTICAL_ALIGN:
            s->valign = (KW("top") || KW("text-top")) ? VA_TOP : KW("middle") ? VA_MIDDLE : (KW("bottom") || KW("text-bottom")) ? VA_BOTTOM :
                        KW("sub") ? VA_SUB : KW("super") ? VA_SUPER : VA_BASELINE;
            break;
        case CP_MARGIN: css_parse_sides(v, n, s->margin, s->fpx, 1); break;
        case CP_PADDING: css_parse_sides(v, n, s->padding, s->fpx, 0); break;
        case CP_MARGIN_TOP: case CP_MARGIN_RIGHT: case CP_MARGIN_BOTTOM: case CP_MARGIN_LEFT: {
            css_len_t l; if (css_parse_len(v, n, &l, s->fpx, 0)) s->margin[prop - CP_MARGIN_TOP] = l;
        } break;
        case CP_PADDING_TOP: case CP_PADDING_RIGHT: case CP_PADDING_BOTTOM: case CP_PADDING_LEFT: {
            css_len_t l; if (css_parse_len(v, n, &l, s->fpx, 0)) { if (css_len_is_auto(l)) l = css_len_px(0); s->padding[prop - CP_PADDING_TOP] = l; }
        } break;
        case CP_BORDER: css_apply_border(s, 15, v, n); break;
        case CP_BORDER_TOP: css_apply_border(s, 1, v, n); break;
        case CP_BORDER_RIGHT: css_apply_border(s, 2, v, n); break;
        case CP_BORDER_BOTTOM: css_apply_border(s, 4, v, n); break;
        case CP_BORDER_LEFT: css_apply_border(s, 8, v, n); break;
        case CP_BORDER_WIDTH: {
            css_len_t t[4]; const u8 *p = v, *e = v + n; int k = 0, w[4];
            (void)t;
            while (css_tok(&p, e, &ts, &tn) && k < 4) { w[k] = css_border_width_tok(ts, tn, s->fpx); if (w[k] < 0) return; k++; }
            if (!k) return;
            int o[4] = { w[0], k > 1 ? w[1] : w[0], k > 2 ? w[2] : w[0], k > 3 ? w[3] : (k > 1 ? w[1] : w[0]) };
            for (int i = 0; i < 4; i++) s->bw[i] = (u8)o[i];
        } break;
        case CP_BORDER_STYLE: {
            const u8 *p = v, *e = v + n; int k = 0, w[4];
            while (css_tok(&p, e, &ts, &tn) && k < 4) { w[k] = css_border_style_kw(ts, tn); if (w[k] < 0) return; k++; }
            if (!k) return;
            int o[4] = { w[0], k > 1 ? w[1] : w[0], k > 2 ? w[2] : w[0], k > 3 ? w[3] : (k > 1 ? w[1] : w[0]) };
            for (int i = 0; i < 4; i++) s->bstyle[i] = (u8)o[i];
        } break;
        case CP_BORDER_COLOR: {
            const u8 *p = v, *e = v + n; int k = 0; u32 w[4];
            while (css_tok(&p, e, &ts, &tn) && k < 4) { if (!css_parse_color(ts, tn, &w[k])) return; k++; }
            if (!k) return;
            u32 o[4] = { w[0], k > 1 ? w[1] : w[0], k > 2 ? w[2] : w[0], k > 3 ? w[3] : (k > 1 ? w[1] : w[0]) };
            for (int i = 0; i < 4; i++) css_set_color(&s->bcolor[i], o[i], s->color);
        } break;
        case CP_BORDER_TOP_WIDTH: case CP_BORDER_RIGHT_WIDTH: case CP_BORDER_BOTTOM_WIDTH: case CP_BORDER_LEFT_WIDTH: {
            int w = css_border_width_tok(v, n, s->fpx); if (w >= 0) s->bw[prop - CP_BORDER_TOP_WIDTH] = (u8)w;
        } break;
        case CP_BORDER_TOP_STYLE: case CP_BORDER_RIGHT_STYLE: case CP_BORDER_BOTTOM_STYLE: case CP_BORDER_LEFT_STYLE: {
            int w = css_border_style_kw(v, n); if (w >= 0) s->bstyle[prop - CP_BORDER_TOP_STYLE] = (u8)w;
        } break;
        case CP_BORDER_TOP_COLOR: case CP_BORDER_RIGHT_COLOR: case CP_BORDER_BOTTOM_COLOR: case CP_BORDER_LEFT_COLOR: {
            u32 c; if (css_parse_color(v, n, &c)) css_set_color(&s->bcolor[prop - CP_BORDER_TOP_COLOR], c, s->color);
        } break;
        case CP_WIDTH: case CP_HEIGHT: case CP_MIN_WIDTH: case CP_MAX_WIDTH: case CP_MIN_HEIGHT: case CP_MAX_HEIGHT: {
            css_len_t l;
            if (!css_parse_len(v, n, &l, s->fpx, 0)) return;
            if ((prop == CP_MIN_WIDTH || prop == CP_MIN_HEIGHT) && css_len_is_auto(l)) l = css_len_px(0);
            switch (prop) {
                case CP_WIDTH: s->width = l; break;          case CP_HEIGHT: s->height = l; break;
                case CP_MIN_WIDTH: s->minw = l; break;       case CP_MAX_WIDTH: s->maxw = l; break;
                case CP_MIN_HEIGHT: s->minh = l; break;      default: s->maxh = l; break;
            }
        } break;
        case CP_FLOAT: s->floating = KW("left") ? 1 : KW("right") ? 2 : 0; break;
        case CP_CLEAR: s->clear = KW("left") ? 1 : KW("right") ? 2 : KW("both") ? 3 : 0; break;
        case CP_POSITION: s->position = KW("absolute") ? POS_ABSOLUTE : KW("fixed") ? POS_FIXED : (KW("relative") || KW("sticky")) ? POS_RELATIVE : POS_STATIC; break;
        case CP_OVERFLOW: case CP_OVERFLOW_X: case CP_OVERFLOW_Y: s->overflow_hidden = (KW("hidden") || KW("clip")) ? 1 : 0; break;
        case CP_BOX_SIZING: s->box_sizing_border = KW("border-box") ? 1 : 0; break;
        case CP_LIST_STYLE: case CP_LIST_STYLE_TYPE: {
            const u8 *p = v, *e = v + n;
            while (css_tok(&p, e, &ts, &tn)) { int x = css_list_style_kw(ts, tn); if (x >= 0) { s->list_style = (u8)x; break; } }
        } break;
        case CP_BORDER_COLLAPSE: s->border_collapse = KW("collapse") ? 1 : 0; break;
        case CP_BORDER_SPACING: {
            css_len_t a, b; const u8 *p = v, *e = v + n;
            if (css_tok(&p, e, &ts, &tn) && css_parse_len(ts, tn, &a, s->fpx, 0)) {
                b = a;
                if (css_tok(&p, e, &ts, &tn)) css_parse_len(ts, tn, &b, s->fpx, 0);
                s->spacing_x = a.px; s->spacing_y = b.px;
            }
        } break;
        case CP_FLEX: {
            const u8 *p = v, *e = v + n; int nums = 0;
            if (KW("none")) { s->flex_grow = 0; s->flex_shrink = 0; s->flex_basis = css_len_auto(); return; }
            if (KW("auto")) { s->flex_grow = 10; s->flex_shrink = 10; s->flex_basis = css_len_auto(); return; }
            s->flex_grow = 10; s->flex_shrink = 10; s->flex_basis = css_len_px(0);
            while (css_tok(&p, e, &ts, &tn)) {
                int x; u32 c = css_num(ts, tn, &x);
                if (c == tn) { if (nums == 0) s->flex_grow = (short)(x / 10); else if (nums == 1) s->flex_shrink = (short)(x / 10); else s->flex_basis = css_len_px(0); nums++; }
                else { css_len_t l; if (css_parse_len(ts, tn, &l, s->fpx, 0)) s->flex_basis = l; }
            }
        } break;
        case CP_FLEX_GROW: { int x; if (css_num(v, n, &x)) s->flex_grow = (short)(x / 10); } break;
        case CP_FLEX_SHRINK: { int x; if (css_num(v, n, &x)) s->flex_shrink = (short)(x / 10); } break;
        case CP_FLEX_BASIS: { css_len_t l; if (css_parse_len(v, n, &l, s->fpx, 0)) s->flex_basis = l; } break;
        case CP_FLEX_DIRECTION: s->flex_dir = (KW("column") || KW("column-reverse")) ? FD_COLUMN : FD_ROW; break;
        case CP_FLEX_WRAP: s->flex_wrap = (KW("wrap") || KW("wrap-reverse")) ? 1 : 0; break;
        case CP_FLEX_FLOW: {
            const u8 *p = v, *e = v + n;
            while (css_tok(&p, e, &ts, &tn)) {
                if (css_kw(ts, tn, "column") || css_kw(ts, tn, "column-reverse")) s->flex_dir = FD_COLUMN;
                else if (css_kw(ts, tn, "row") || css_kw(ts, tn, "row-reverse")) s->flex_dir = FD_ROW;
                else if (css_kw(ts, tn, "wrap") || css_kw(ts, tn, "wrap-reverse")) s->flex_wrap = 1;
                else if (css_kw(ts, tn, "nowrap")) s->flex_wrap = 0;
            }
        } break;
        case CP_JUSTIFY_CONTENT:
            s->justify = (KW("flex-end") || KW("end") || KW("right")) ? JC_END : KW("center") ? JC_CENTER : KW("space-between") ? JC_BETWEEN :
                         KW("space-around") ? JC_AROUND : KW("space-evenly") ? JC_EVENLY : JC_START;
            break;
        case CP_ALIGN_ITEMS: { int a = css_flex_align_kw(v, n, 0); if (a >= 0) s->align_items = (u8)a; } break;
        case CP_ALIGN_SELF: { int a = css_flex_align_kw(v, n, 1); if (a >= 0) s->align_self = (u8)a; } break;
        case CP_GAP: case CP_GRID_GAP: {
            css_len_t a, b; const u8 *p = v, *e = v + n;
            if (css_tok(&p, e, &ts, &tn) && css_parse_len(ts, tn, &a, s->fpx, 0)) {
                b = a;
                if (css_tok(&p, e, &ts, &tn)) css_parse_len(ts, tn, &b, s->fpx, 0);
                s->gap_row = css_len_is_auto(a) ? 0 : a.px; s->gap_col = css_len_is_auto(b) ? 0 : b.px;
            }
        } break;
        case CP_ROW_GAP: { css_len_t l; if (css_parse_len(v, n, &l, s->fpx, 0)) s->gap_row = css_len_is_auto(l) ? 0 : l.px; } break;
        case CP_COLUMN_GAP: { css_len_t l; if (css_parse_len(v, n, &l, s->fpx, 0)) s->gap_col = css_len_is_auto(l) ? 0 : l.px; } break;
        case CP_GRID_TEMPLATE_COLUMNS:
            if (v >= RD_POOL && v < RD_POOL + RD_POOL_SIZE) { s->gtc_off = (u32)(v - RD_POOL); s->gtc_len = (u16)(n > 65535 ? 65535 : n); }
            else if (KW("none")) s->gtc_len = 0;
            break;
        case CP_TOP: case CP_LEFT: case CP_RIGHT: case CP_BOTTOM: {
            css_len_t l; if (css_parse_len(v, n, &l, s->fpx, 0) && !l.pct && l.px <= -300) s->pad8 |= 1;     /* parked off-screen */
        } break;
        case CP_CLIP: if (KW("rect(0,0,0,0)") || KW("rect(0 0 0 0)") || KW("rect(1px,1px,1px,1px)") || KW("rect(1px 1px 1px 1px)")) s->pad8 |= 2; break;
        case CP_CLIP_PATH: if (n > 5 && css_kw(v, 5, "inset")) s->pad8 |= 2; break;
        default: break;
    }
    #undef KW
}

/* ============================================================
 * presentational attributes (HTML4 leftovers that millions of pages still use)
 * ============================================================ */
/* "120", "120px", "50%" -> css_len_t */
static inline int css_attr_len(const u8 *v, u32 n, css_len_t *out) {
    int x; u32 c = css_num(v, n, &x);
    if (!c || x < 0) return 0;
    if (c < n && v[c] == '%') { out->px = 0; out->pct = (short)(x / 10 > 30000 ? 30000 : x / 10); return 1; }
    int px = (x * CSS_ZOOM_NUM + (CSS_ZOOM_DEN * 100) / 2) / (CSS_ZOOM_DEN * 100);
    if (x > 0 && px < 1) px = 1;
    *out = css_len_px(px); return 1;
}
static inline int css_zoom_px(int css_px) { int p = (css_px * CSS_ZOOM_NUM + CSS_ZOOM_DEN / 2) / CSS_ZOOM_DEN; return (css_px > 0 && p < 1) ? 1 : p; }

static inline void css_hints(u32 node, css_style_t *s, const css_style_t *par) {
    rd_node_t *n = &RD_NODES[node];
    u32 tag = n->tag;
    (void)par;
    for (u32 i = 0; i < n->b; i++) {
        rd_attr_t *a = &RD_ATTRS[n->a + i];
        const u8 *v = RD_POOL + a->voff; u32 vl = a->vlen;
        u32 c;
        switch (a->id) {
            case AT_ALIGN:
                if (tag == TG_IMG || tag == TG_INPUT || tag == TG_OBJECT || tag == TG_EMBED) break;
                if (css_kw(v, vl, "center") || css_kw(v, vl, "middle")) {
                    s->text_align = TA_CENTER;
                    if (tag == TG_TABLE || tag == TG_HR) { s->margin[1] = s->margin[3] = css_len_auto(); }
                } else if (css_kw(v, vl, "right")) {
                    s->text_align = TA_RIGHT;
                    if (tag == TG_TABLE || tag == TG_HR) s->margin[3] = css_len_auto();
                } else if (css_kw(v, vl, "left")) s->text_align = TA_LEFT;
                else if (css_kw(v, vl, "justify")) s->text_align = TA_JUSTIFY;
                break;
            case AT_VALIGN:
                if (tag == TG_TD || tag == TG_TH || tag == TG_TR || tag == TG_TBODY || tag == TG_THEAD || tag == TG_TFOOT)
                    s->valign = css_kw(v, vl, "top") ? VA_TOP : css_kw(v, vl, "bottom") ? VA_BOTTOM : css_kw(v, vl, "baseline") ? VA_BASELINE : VA_MIDDLE;
                break;
            case AT_BGCOLOR:
                if (tag == TG_BODY || tag == TG_TABLE || tag == TG_TR || tag == TG_TD || tag == TG_TH || tag == TG_THEAD || tag == TG_TBODY) {
                    if (css_parse_color(v, vl, &c)) s->bg = c;
                    else if (vl == 6 && css_hexv(v[0]) >= 0) {                      /* "FFCC00" without the '#': browsers accept it */
                        u8 t[8]; t[0] = '#'; for (u32 k = 0; k < 6; k++) t[1 + k] = v[k];
                        if (css_parse_color(t, 7, &c)) s->bg = c;
                    }
                }
                break;
            case AT_TEXT: if (tag == TG_BODY && css_parse_color(v, vl, &c)) s->color = c; break;
            case AT_COLOR:
                if (tag == TG_FONT || tag == TG_HR) {
                    if (css_parse_color(v, vl, &c)) s->color = c;
                    else if (vl == 6 && css_hexv(v[0]) >= 0) { u8 t[8]; t[0] = '#'; for (u32 k = 0; k < 6; k++) t[1 + k] = v[k]; if (css_parse_color(t, 7, &c)) s->color = c; }
                }
                break;
            case AT_SIZE:
                if (tag == TG_FONT && vl) {
                    static const short map[8] = { 16, 10, 13, 16, 18, 24, 32, 48 };
                    int x, lvl;
                    if (v[0] == '+' || v[0] == '-') { if (!css_num(v + 1, vl - 1, &x)) break; lvl = 3 + (v[0] == '-' ? -(x / 100) : (x / 100)); }
                    else { if (!css_num(v, vl, &x)) break; lvl = x / 100; }
                    if (lvl < 1) lvl = 1; if (lvl > 7) lvl = 7;
                    css_set_fpx(s, map[lvl]);
                }
                break;
            case AT_WIDTH: case AT_HEIGHT:
                if (tag == TG_TABLE || tag == TG_TD || tag == TG_TH || tag == TG_IMG || tag == TG_HR || tag == TG_IFRAME || tag == TG_CANVAS ||
                    tag == TG_VIDEO || tag == TG_SVG || tag == TG_OBJECT || tag == TG_EMBED || tag == TG_AUDIO || tag == TG_COL || tag == TG_TR) {
                    css_len_t l;
                    if (css_attr_len(v, vl, &l)) { if (a->id == AT_WIDTH) s->width = l; else s->height = l; }
                }
                break;
            case AT_NOWRAP: if (tag == TG_TD || tag == TG_TH) s->white_space = WS_NOWRAP; break;
            case AT_HSPACE: if (tag == TG_IMG) { css_len_t l; if (css_attr_len(v, vl, &l)) { s->margin[1] = s->margin[3] = l; } } break;
            case AT_VSPACE: if (tag == TG_IMG) { css_len_t l; if (css_attr_len(v, vl, &l)) { s->margin[0] = s->margin[2] = l; } } break;
            case AT_BORDER:
                if (tag == TG_TABLE || tag == TG_IMG) {
                    int x = 1; if (vl) { if (!css_num(v, vl, &x)) x = 100; }
                    int w = x > 0 ? css_zoom_px(x / 100) : 0;
                    for (int k = 0; k < 4; k++) {
                        s->bw[k] = (u8)w; s->bstyle[k] = (u8)(w ? (tag == TG_TABLE ? BS_OUTSET : BS_SOLID) : BS_NONE);
                        s->bcolor[k] = tag == TG_TABLE ? 0x808080u : CSS_CURRENT;
                    }
                }
                break;
            case AT_TYPE:                                  /* <ol type="a">, <ul type=square>, <li type=i> */
                if (tag == TG_OL || tag == TG_UL || tag == TG_LI) {
                    if (vl == 1 && v[0] == 'a') s->list_style = LS_LOWER_ALPHA; else if (vl == 1 && v[0] == 'A') s->list_style = LS_UPPER_ALPHA;
                    else if (vl == 1 && v[0] == 'i') s->list_style = LS_LOWER_ROMAN; else if (vl == 1 && v[0] == 'I') s->list_style = LS_UPPER_ROMAN;
                    else if (vl == 1 && v[0] == '1') s->list_style = LS_DECIMAL;
                    else { int x = css_list_style_kw(v, vl); if (x >= 0) s->list_style = (u8)x; }
                }
                break;
            case AT_CELLSPACING: if (tag == TG_TABLE) { int x; if (css_num(v, vl, &x)) s->spacing_x = s->spacing_y = (short)css_zoom_px(x / 100); } break;
            default: break;
        }
    }
    if (tag == TG_TD || tag == TG_TH) {                         /* the table's border=/cellpadding= reach down into its cells */
        u32 t = n->parent;
        for (int up = 0; t && up < 6 && RD_NODES[t].tag != TG_TABLE; up++) t = RD_NODES[t].parent;
        if (t && RD_NODES[t].tag == TG_TABLE) {
            u32 vl; const u8 *v = dom_attr(t, AT_BORDER, &vl);
            int x = 0;
            if (v) { x = 100; if (vl) { if (!css_num(v, vl, &x)) x = 100; } }
            if (x > 0) for (int k = 0; k < 4; k++) { s->bw[k] = 1; s->bstyle[k] = BS_INSET; s->bcolor[k] = 0x808080u; }
            v = dom_attr(t, AT_CELLPADDING, &vl);
            if (v && css_num(v, vl, &x)) { int p = css_zoom_px(x / 100); for (int k = 0; k < 4; k++) s->padding[k] = css_len_px(p); }
        }
    }
}

/* ============================================================
 * the cascade
 * ============================================================ */
static inline void css_apply_inline(u32 node, css_style_t *s, const css_style_t *par, int want_important) {
    u32 len; const u8 *v = dom_attr(node, AT_STYLE, &len);
    if (!v) return;
    u32 p = (u32)(v - RD_POOL), end = p + len, ns, nn, vs, vn; int imp;
    while (css_next_decl(RD_POOL, &p, end, &ns, &nn, &vs, &vn, &imp)) {
        if (imp != want_important) continue;
        u32 pid = css_prop_id(RD_POOL + ns, nn);
        if (pid != CP_NONE) css_apply(s, pid, RD_POOL + vs, vn, par);
    }
}

#define CSS_MAX_MATCH 192
static inline void css_collect(u16 head, u32 node, u16 *m, int *nm) {
    for (u16 r = head; r != CSS_NIL; r = RD_CSS_RULES[r].next) {
        css_rule_t *rule = &RD_CSS_RULES[r];
        if (*nm >= CSS_MAX_MATCH) return;
        if (css_match_from(node, &RD_CSS_COMPS[rule->comp_first], (int)rule->ncomp - 1)) m[(*nm)++] = r;
    }
}

/* Computes the style of element `node` given its parent's computed style (NULL for the root). */
static inline void css_compute(u32 node, const css_style_t *par, css_style_t *s) {
    rd_node_t *n = &RD_NODES[node];
    css_style_inherit(s, par);

    u16 m[CSS_MAX_MATCH]; int nm = 0;
    css_collect(css_universal, node, m, &nm);
    css_collect(css_by_tag[n->tag], node, m, &nm);
    u32 len; const u8 *v = dom_attr(node, AT_CLASS, &len);
    if (v) {
        for (u32 i = 0; i < len; ) {
            while (i < len && css_is_ws(v[i])) i++;
            u32 st = i; while (i < len && !css_is_ws(v[i])) i++;
            if (i > st) css_collect(css_by_cls[css_hash(v + st, i - st) & (CSS_BUCKET_N - 1)], node, m, &nm);
        }
    }
    v = dom_attr(node, AT_ID, &len);
    if (v) css_collect(css_by_id[css_hash(v, len) & (CSS_BUCKET_N - 1)], node, m, &nm);

    for (int i = 1; i < nm; i++) {                                  /* insertion sort: lowest priority first */
        u16 x = m[i]; int j = i - 1;
        while (j >= 0 && (RD_CSS_RULES[m[j]].spec > RD_CSS_RULES[x].spec ||
              (RD_CSS_RULES[m[j]].spec == RD_CSS_RULES[x].spec && RD_CSS_RULES[m[j]].order > RD_CSS_RULES[x].order))) { m[j + 1] = m[j]; j--; }
        m[j + 1] = x;
    }
    int hints_done = 0;                                             /* presentational attributes slot in between: UA < hints < author */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nm; i++) {
            css_rule_t *r = &RD_CSS_RULES[m[i]];
            if (pass == 0 && !hints_done && (r->spec >> 28) >= 1) { css_hints(node, s, par); hints_done = 1; }
            for (u32 d = 0; d < r->ndecl; d++) {
                css_decl_t *dc = &RD_CSS_DECLS[r->decl_first + d];
                if (dc->imp == pass) css_apply(s, dc->prop, RD_POOL + dc->voff, dc->vlen, par);
            }
        }
        if (pass == 0 && !hints_done) { css_hints(node, s, par); hints_done = 1; }
        css_apply_inline(node, s, par, pass);
    }

    /* finishing touches */
    for (int i = 0; i < 4; i++) {
        if (s->bstyle[i] == BS_NONE) s->bw[i] = 0;
        if (s->bcolor[i] == CSS_CURRENT) s->bcolor[i] = s->color;
    }
    if (s->bg == CSS_CURRENT) s->bg = s->color;
    if (s->floating && (s->display == DISP_INLINE || s->display == DISP_BLOCK || s->display == DISP_LIST_ITEM)) s->display = DISP_INLINE_BLOCK;
    if ((s->pad8 & 3) && s->position != POS_STATIC) s->display = DISP_NONE;      /* sr-only / off-screen tricks */
    if (s->position == POS_ABSOLUTE || s->position == POS_FIXED) { if (s->display == DISP_INLINE) s->display = DISP_BLOCK; }
}

/* Sets up the engine's stylesheets for the document just parsed: UA sheet, then every <style> in order. */
static inline void css_load_document(void) {
    css_reset();
    u32 off = dom_pool_len;
    for (u32 i = 0; css_ua_sheet[i]; i++) dom_pool_putc((u8)css_ua_sheet[i]);
    css_parse_sheet(off, dom_pool_len - off, 0);
    for (u32 nd = 1; nd < dom_node_count; nd++) {
        rd_node_t *e = &RD_NODES[nd];
        if (e->kind != RDK_ELEM || e->tag != TG_STYLE) continue;
        u32 ml; const u8 *mv = dom_attr(nd, AT_MEDIA, &ml);
        if (mv && !css_media_eval(mv, ml)) continue;
        u32 tl; const u8 *tv = dom_attr(nd, AT_TYPE, &tl);
        if (tv && tl && !css_kw(tv, tl, "text/css")) continue;
        for (u32 c = e->first; c; c = RD_NODES[c].next)
            if (RD_NODES[c].kind == RDK_TEXT) css_parse_sheet(RD_NODES[c].a, RD_NODES[c].b, 1);
    }
}

#endif
