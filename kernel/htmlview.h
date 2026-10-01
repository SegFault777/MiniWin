#ifndef HTMLVIEW_H
#define HTMLVIEW_H
#include "io.h"
#include "memmap.h"

/* ============================================================
 * htmlview.h -- turns an HTML page into something readable in a
 * monospaced character grid: the body text, in order, with markup
 * removed, entities decoded, whitespace collapsed the way a browser
 * would, block elements starting new lines, table rows on their own
 * lines, and every <a href> remembered so it can be clicked.
 *
 * WHY: MiniWeb used to draw the raw response bytes, so a page showed up
 * as "<!DOCTYPE html><html><head><meta ..." -- unreadable even when the
 * network side worked. This is deliberately NOT a browser engine: no
 * CSS, no images, no JavaScript, no forms, no tables-as-grids. It is
 * "what would you get from `lynx -dump`", which is exactly what a
 * JavaScript-free site like DuckDuckGo Lite is designed to be read as.
 *
 * TWO STAGES, both pure logic (no drawing, so a host-side test can run
 * them):
 *   1. hv_render_html()  -> HV_TEXT: the visible text, '\n' between lines,
 *                           plus the link table (which byte ranges are
 *                           links, and where each points).
 *   2. hv_layout(cols)   -> HV_LINES: where each on-screen line starts once
 *                           the text is wrapped to `cols` character cells.
 *                           Re-run whenever the window's width changes.
 * kernel.c does the drawing (font, colors, scrollbar, hit-testing).
 *
 * CHARACTERS: one "cell" is one column. ASCII is one byte; a Hangul
 * syllable/jamo is kept as its 3-byte UTF-8 form (kernel/font_ko.h can
 * draw it); every other non-ASCII character is mapped to a plain-ASCII
 * lookalike where one exists (curly quotes, dashes, ellipsis, nbsp...)
 * and otherwise becomes '?'. Any byte >= 0xE0 in HV_TEXT therefore
 * always starts a 3-byte Hangul cell.
 * ============================================================ */

typedef struct { u32 tstart, tend, url_off; } hv_link_t;

#ifndef HV_TEXT      /* (a host-side unit test defines its own arrays before including this) */
#define HV_TEXT_SIZE  MW_HV_TEXT_SIZE
#define HV_URLS_SIZE  MW_HV_URLS_SIZE
#define HV_MAX_LINKS  (MW_HV_LINKS_SIZE / 12)
#define HV_MAX_LINES  (MW_HV_LINES_SIZE / 4)
#endif

#ifndef HV_TEXT
#define HV_TEXT  ((u8 *)MW_HV_TEXT_ADDR)
#define HV_URLS  ((char *)MW_HV_URLS_ADDR)
#define HV_LINKS ((hv_link_t *)MW_HV_LINKS_ADDR)
#define HV_LINES ((u32 *)MW_HV_LINES_ADDR)
#endif

typedef struct {
    u32 text_len;
    u32 link_count;
    u32 url_len;
    u32 line_count;
    u32 wrap_cols;
    u32 scroll;              /* index of the first visible line (owned by the UI) */
    int truncated;
    char title[96];
} hv_t;

static hv_t hv;

/* ---- parser state (file-private) ---- */
static u32 hv_nl;            /* newlines wanted before the next visible character (0..2) */
static int hv_space;         /* a collapsed whitespace run is pending */
static int hv_pre;           /* inside <pre>: keep whitespace */
static int hv_bullet;        /* a list item just began: prefix its first character with "* " */
static int hv_row_text;      /* the current table row has produced visible text */
static int hv_link_open;     /* inside <a href=...> */
static int hv_link_idx;      /* index of the link record for the open anchor, or -1 until its first character */
static u32 hv_link_url;      /* url pool offset for the open anchor */

static inline void hv_clear(void) {
    hv.text_len = 0; hv.link_count = 0; hv.url_len = 0; hv.line_count = 0;
    hv.wrap_cols = 0; hv.scroll = 0; hv.truncated = 0; hv.title[0] = 0;
    hv_nl = 0; hv_space = 0; hv_pre = 0; hv_bullet = 0; hv_row_text = 0;
    hv_link_open = 0; hv_link_idx = -1; hv_link_url = 0;
}

static inline void hv_put(u8 b) {
    if (hv.text_len + 4 < HV_TEXT_SIZE) HV_TEXT[hv.text_len++] = b;
    else hv.truncated = 1;
}
static inline int hv_at_line_start(void) {
    return hv.text_len == 0 || HV_TEXT[hv.text_len - 1] == '\n';
}

/* Emits one visible cell (`n` = 1 for ASCII, 3 for a Hangul character), first flushing whatever
 * newlines / spaces / list bullets were pending, and opening the link record if this is the first
 * character of an anchor. */
static inline void hv_emit_cell(const u8 *bytes, int n) {
    if (hv.text_len > 0) {
        for (u32 k = 0; k < hv_nl; k++) hv_put('\n');
    }
    hv_nl = 0;
    if (hv_space && !hv_at_line_start()) hv_put(' ');
    hv_space = 0;
    if (hv_bullet) { hv_put('*'); hv_put(' '); hv_bullet = 0; }
    if (hv_link_open && hv_link_idx < 0 && hv.link_count < HV_MAX_LINKS) {
        hv_link_idx = (int)hv.link_count++;
        HV_LINKS[hv_link_idx].tstart = hv.text_len;
        HV_LINKS[hv_link_idx].tend = hv.text_len;
        HV_LINKS[hv_link_idx].url_off = hv_link_url;
    }
    for (int i = 0; i < n; i++) hv_put(bytes[i]);
    if (hv_link_idx >= 0) HV_LINKS[hv_link_idx].tend = hv.text_len;
    hv_row_text = 1;
}
static inline void hv_emit_ascii(char c) { u8 b = (u8)c; hv_emit_cell(&b, 1); }
static inline void hv_emit_str(const char *s) { while (*s) hv_emit_ascii(*s++); }

static inline int hv_is_hangul(u32 cp) {
    return (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F);
}

/* Feeds one decoded character (a Unicode code point) into the text stream. */
static inline void hv_text_cp(u32 cp) {
    if (cp == 0xA0) cp = ' ';                                 /* nbsp is a space that doesn't count as text */
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f') {
        if (hv_pre) {
            if (cp == '\n') { if (hv.text_len > 0) hv_put('\n'); }
            else if (cp == '\t') { hv_emit_str("    "); }
            else if (cp == ' ') { hv_emit_ascii(' '); }
        } else {
            hv_space = 1;
        }
        return;
    }
    if (cp < 0x20 || cp == 0x7F) return;                      /* control characters vanish */
    if (cp < 0x80) { hv_emit_ascii((char)cp); return; }
    if (hv_is_hangul(cp)) {
        u8 b[3] = { (u8)(0xE0 | (cp >> 12)), (u8)(0x80 | ((cp >> 6) & 0x3F)), (u8)(0x80 | (cp & 0x3F)) };
        hv_emit_cell(b, 3);
        return;
    }
    switch (cp) {                                             /* ASCII look-alikes for common typography */
        case 0x2013: case 0x2014: case 0x2212: hv_emit_ascii('-'); return;
        case 0x2018: case 0x2019: case 0x2032: hv_emit_ascii('\''); return;
        case 0x201C: case 0x201D: case 0x2033: hv_emit_ascii('"'); return;
        case 0x2022: case 0x25CF: case 0x25AA: hv_emit_ascii('*'); return;
        case 0x2026: hv_emit_str("..."); return;
        case 0xB7: case 0x2027: hv_emit_ascii('.'); return;
        case 0xA9: hv_emit_str("(c)"); return;
        case 0xAE: hv_emit_str("(R)"); return;
        case 0x2122: hv_emit_str("(TM)"); return;
        case 0xAB: hv_emit_str("<<"); return;
        case 0xBB: hv_emit_str(">>"); return;
        case 0x2190: hv_emit_str("<-"); return;
        case 0x2192: hv_emit_str("->"); return;
        case 0xD7: hv_emit_ascii('x'); return;
        case 0xB0: hv_emit_ascii('o'); return;
        case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: return;   /* zero-width: invisible */
        default: hv_emit_ascii('?'); return;
    }
}

/* ---- entities ---- */
static inline int hv_streq_n(const u8 *s, u32 n, const char *lit) {
    u32 i = 0;
    for (; lit[i]; i++) if (i >= n || s[i] != (u8)lit[i]) return 0;
    return i == n;
}
/* Decodes an entity starting just AFTER the '&' at s[0..avail). On success returns the number of
 * bytes consumed (through the ';') and stores the code point; on failure returns 0 (the '&' is
 * then literal text). */
static inline u32 hv_entity(const u8 *s, u32 avail, u32 *cp_out) {
    u32 semi = 0;
    while (semi < avail && semi < 10 && s[semi] != ';') semi++;
    if (semi >= avail || s[semi] != ';' || semi == 0) return 0;
    if (s[0] == '#') {
        u32 v = 0; u32 i = 1;
        if (i < semi && (s[i] == 'x' || s[i] == 'X')) {
            i++;
            if (i == semi) return 0;
            for (; i < semi; i++) {
                u8 c = s[i]; u32 d;
                if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10; else return 0;
                v = v * 16 + d;
            }
        } else {
            if (i == semi) return 0;
            for (; i < semi; i++) { if (s[i] < '0' || s[i] > '9') return 0; v = v * 10 + (u32)(s[i] - '0'); }
        }
        *cp_out = v; return semi + 1;
    }
    static const struct { const char *name; u32 cp; } table[] = {
        {"amp",'&'},{"lt",'<'},{"gt",'>'},{"quot",'"'},{"apos",'\''},{"nbsp",0xA0},{"copy",0xA9},{"reg",0xAE},
        {"trade",0x2122},{"laquo",0xAB},{"raquo",0xBB},{"middot",0xB7},{"hellip",0x2026},{"mdash",0x2014},
        {"ndash",0x2013},{"lsquo",0x2018},{"rsquo",0x2019},{"ldquo",0x201C},{"rdquo",0x201D},{"bull",0x2022},
        {"times",0xD7},{"deg",0xB0},{"larr",0x2190},{"rarr",0x2192},{"shy",0x200B},{"zwnj",0x200C},
    };
    for (unsigned k = 0; k < sizeof(table) / sizeof(table[0]); k++) {
        if (hv_streq_n(s, semi, table[k].name)) { *cp_out = table[k].cp; return semi + 1; }
    }
    return 0;
}

/* Decodes one UTF-8 character at p[0..avail); returns bytes consumed (>= 1) and the code point.
 * Malformed input yields U+FFFD for one byte at a time, so garbage can't derail the parse. */
static inline u32 hv_utf8(const u8 *p, u32 avail, u32 *cp) {
    u8 c = p[0];
    if (c < 0x80) { *cp = c; return 1; }
    u32 need = (c >= 0xF0 && c < 0xF8) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC2 && c < 0xE0) ? 2 : 0;
    if (c >= 0xF8 || need == 0 || need > avail) { *cp = 0xFFFD; return 1; }
    u32 v = need == 2 ? (c & 0x1F) : need == 3 ? (c & 0x0F) : (c & 0x07);
    for (u32 i = 1; i < need; i++) {
        if ((p[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (p[i] & 0x3F);
    }
    *cp = v; return need;
}

static inline char hv_lc(u8 c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c; }

/* ---- tags ---- */
static inline int hv_name_is(const char *name, const char *lit) {
    u32 i = 0;
    while (lit[i]) { if (name[i] != lit[i]) return 0; i++; }
    return name[i] == 0;
}

/* Handles one complete opening or closing tag. `href` is the (already entity-decoded) href
 * attribute of an <a>, or "" if none. */
static inline void hv_tag(const char *name, int closing, const char *href) {
    /* --- inline elements that matter --- */
    if (name[0] == 'a' && name[1] == 0) {
        if (closing) {
            hv_link_open = 0; hv_link_idx = -1;
        } else if (href[0] && hv.url_len + 512 < HV_URLS_SIZE) {
            hv_link_open = 1; hv_link_idx = -1;
            hv_link_url = hv.url_len;
            u32 n = 0;
            while (href[n] && n < 500) { HV_URLS[hv.url_len++] = href[n]; n++; }
            HV_URLS[hv.url_len++] = 0;
        }
        return;
    }
    if (hv_name_is(name, "br")) { hv_nl = hv_nl >= 1 ? 2 : 1; return; }
    if (hv_name_is(name, "pre")) {
        hv_pre = closing ? 0 : 1;
        if (hv_nl < 1) hv_nl = 1;
        return;
    }
    if (hv_name_is(name, "li")) {
        if (!closing) { if (hv_nl < 1) hv_nl = 1; hv_bullet = 1; }
        return;
    }
    if (hv_name_is(name, "hr")) {
        if (hv_nl < 1) hv_nl = 1;
        for (int i = 0; i < 24; i++) hv_emit_ascii('-');
        hv_nl = 1;
        return;
    }
    if (hv_name_is(name, "td") || hv_name_is(name, "th")) {
        if (!closing) hv_space = 1;              /* cells on one row are separated by a space */
        return;
    }
    if (hv_name_is(name, "tr")) {
        if (!closing) { if (hv_nl < 1) hv_nl = 1; hv_row_text = 0; }
        else if (!hv_row_text) hv_nl = 2;         /* a row with no visible text is a spacer: leave a blank line */
        return;
    }
    /* --- paragraph-level: a blank line around them --- */
    if (name[0] == 'p' && name[1] == 0) { if (hv_nl < 2) hv_nl = 2; return; }
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == 0) { if (hv_nl < 2) hv_nl = 2; return; }
    if (hv_name_is(name, "table") || hv_name_is(name, "blockquote") || hv_name_is(name, "ul") || hv_name_is(name, "ol") ||
        hv_name_is(name, "dl") || hv_name_is(name, "form")) { if (hv_nl < 1) hv_nl = 1; return; }
    /* --- line-level blocks --- */
    static const char *const blocks[] = { "div", "center", "section", "article", "header", "footer", "nav", "main",
        "aside", "address", "fieldset", "caption", "dt", "dd", "tbody", "thead", "tfoot", "figure", "figcaption", "details", "summary" };
    for (unsigned k = 0; k < sizeof(blocks) / sizeof(blocks[0]); k++) {
        if (hv_name_is(name, blocks[k])) { if (hv_nl < 1) hv_nl = 1; return; }
    }
}

/* Finds "</name" (case-insensitive) at or after i, and returns the offset just past its '>'. */
static inline u32 hv_skip_element(const u8 *h, u32 len, u32 i, const char *name) {
    u32 nl = 0; while (name[nl]) nl++;
    for (; i + 2 + nl < len; i++) {
        if (h[i] != '<' || h[i + 1] != '/') continue;
        u32 k = 0;
        while (k < nl && hv_lc(h[i + 2 + k]) == name[k]) k++;
        if (k == nl) {
            u32 j = i + 2 + nl;
            while (j < len && h[j] != '>') j++;
            return j < len ? j + 1 : len;
        }
    }
    return len;
}

/* The main pass: HTML in, HV_TEXT + link table out. */
static inline void hv_render_html(const u8 *h, u32 len) {
    hv_clear();
    u32 i = 0;
    int in_title = 0;
    while (i < len && !hv.truncated) {
        u8 c = h[i];
        if (c == '<') {
            /* comment / doctype / CDATA / processing instruction */
            if (i + 3 < len && h[i+1] == '!' && h[i+2] == '-' && h[i+3] == '-') {
                i += 4;
                while (i + 2 < len && !(h[i] == '-' && h[i+1] == '-' && h[i+2] == '>')) i++;
                i = (i + 3 <= len) ? i + 3 : len;
                continue;
            }
            if (i + 1 < len && (h[i+1] == '!' || h[i+1] == '?')) {
                while (i < len && h[i] != '>') i++;
                i++;
                continue;
            }
            int closing = (i + 1 < len && h[i+1] == '/');
            u32 ns = i + 1 + (closing ? 1 : 0);
            if (ns >= len || !((h[ns] >= 'A' && h[ns] <= 'Z') || (h[ns] >= 'a' && h[ns] <= 'z'))) {
                hv_text_cp('<'); i++; continue;          /* a lone '<' in text */
            }
            char name[16]; u32 nn = 0;
            u32 j = ns;
            while (j < len && !(h[j] == ' ' || h[j] == '\t' || h[j] == '\r' || h[j] == '\n' || h[j] == '/' || h[j] == '>')) {
                if (nn < sizeof(name) - 1) name[nn++] = hv_lc(h[j]);
                j++;
            }
            name[nn] = 0;

            /* attributes: we only need href (on <a>); quotes may legitimately contain '>' */
            char href[512]; href[0] = 0;
            int self_close = 0;
            while (j < len && h[j] != '>') {
                while (j < len && (h[j] == ' ' || h[j] == '\t' || h[j] == '\r' || h[j] == '\n')) j++;
                if (j >= len || h[j] == '>') break;
                if (h[j] == '/') { self_close = 1; j++; continue; }
                self_close = 0;
                char an[12]; u32 an_n = 0;
                while (j < len && !(h[j] == '=' || h[j] == ' ' || h[j] == '\t' || h[j] == '\r' || h[j] == '\n' || h[j] == '>' || h[j] == '/')) {
                    if (an_n < sizeof(an) - 1) an[an_n++] = hv_lc(h[j]);
                    j++;
                }
                an[an_n] = 0;
                while (j < len && (h[j] == ' ' || h[j] == '\t' || h[j] == '\r' || h[j] == '\n')) j++;
                int is_href = (an[0] == 'h' && an[1] == 'r' && an[2] == 'e' && an[3] == 'f' && an[4] == 0);
                if (j < len && h[j] == '=') {
                    j++;
                    while (j < len && (h[j] == ' ' || h[j] == '\t' || h[j] == '\r' || h[j] == '\n')) j++;
                    u8 q = (j < len && (h[j] == '"' || h[j] == '\'')) ? h[j] : 0;
                    if (q) j++;
                    u32 hn = 0;
                    while (j < len && (q ? h[j] != q : !(h[j] == ' ' || h[j] == '\t' || h[j] == '\r' || h[j] == '\n' || h[j] == '>'))) {
                        if (is_href && hn + 1 < sizeof(href)) {
                            if (h[j] == '&') {
                                u32 cp; u32 used = hv_entity(h + j + 1, len - j - 1, &cp);
                                if (used && cp < 0x80) { href[hn++] = (char)cp; j += 1 + used; continue; }
                            }
                            if (h[j] != '\r' && h[j] != '\n' && h[j] != '\t') href[hn++] = (char)h[j];
                        }
                        j++;
                    }
                    if (q && j < len) j++;
                    if (is_href) href[hn] = 0;
                }
            }
            i = (j < len) ? j + 1 : len;

            if (closing) {
                if (hv_name_is(name, "title")) in_title = 0;
                hv_tag(name, 1, "");
                continue;
            }
            if (hv_name_is(name, "script") || hv_name_is(name, "style") || hv_name_is(name, "svg") ||
                hv_name_is(name, "template") || hv_name_is(name, "textarea")) {
                if (!self_close) i = hv_skip_element(h, len, i, name);
                continue;
            }
            if (hv_name_is(name, "title")) { in_title = 1; hv.title[0] = 0; continue; }
            hv_tag(name, 0, href);
            continue;
        }

        /* text */
        u32 cp, used;
        if (c == '&' && (used = hv_entity(h + i + 1, len - i - 1, &cp)) != 0) {
            i += 1 + used;
        } else {
            used = hv_utf8(h + i, len - i, &cp);
            i += used;
        }
        if (in_title) {
            u32 tl = 0; while (hv.title[tl]) tl++;
            if (cp == ' ' || cp == '\n' || cp == '\r' || cp == '\t' || cp == 0xA0) { if (tl > 0 && hv.title[tl-1] != ' ' && tl + 1 < sizeof(hv.title)) { hv.title[tl++] = ' '; hv.title[tl] = 0; } }
            else if (cp >= 0x20 && cp < 0x7F && tl + 1 < sizeof(hv.title)) { hv.title[tl++] = (char)cp; hv.title[tl] = 0; }
            continue;
        }
        hv_text_cp(cp);
    }
    while (hv.text_len > 0 && (HV_TEXT[hv.text_len - 1] == '\n' || HV_TEXT[hv.text_len - 1] == ' ')) hv.text_len--;
    { u32 tl = 0; while (hv.title[tl]) tl++; while (tl > 0 && hv.title[tl-1] == ' ') hv.title[--tl] = 0; }
}

/* Plain text (text/plain, or anything else we choose to show verbatim). */
static inline void hv_render_plain(const u8 *t, u32 len) {
    hv_clear();
    hv_pre = 1;
    for (u32 i = 0; i < len && !hv.truncated; ) {
        u32 cp; u32 used = hv_utf8(t + i, len - i, &cp);
        i += used;
        if (cp == '\r') continue;
        hv_text_cp(cp);
    }
    while (hv.text_len > 0 && HV_TEXT[hv.text_len - 1] == '\n') hv.text_len--;
}
static inline void hv_render_message(const char *msg) {
    hv_clear();
    hv_pre = 1;
    for (const char *p = msg; *p; p++) hv_text_cp((u8)*p);
}

/* ---- layout: wrap HV_TEXT into lines of at most `cols` cells ---- */
static inline u32 hv_cell_len(u8 c) { return c >= 0xE0 ? 3 : 1; }

static inline void hv_layout(u32 cols) {
    if (cols < 8) cols = 8;
    hv.wrap_cols = cols;
    hv.line_count = 0;
    u32 pos = 0;
    while (pos <= hv.text_len && hv.line_count < HV_MAX_LINES) {
        HV_LINES[hv.line_count++] = pos;
        u32 cells = 0, p = pos, last_space = 0;
        int have_space = 0;
        for (;;) {
            if (p >= hv.text_len) { pos = hv.text_len + 1; break; }
            u8 c = HV_TEXT[p];
            if (c == '\n') { pos = p + 1; break; }
            if (cells == cols) {
                if (c == ' ') { pos = p + 1; break; }               /* the break falls on a space: drop it */
                if (have_space) { pos = last_space + 1; break; }    /* back up to the last space in this line */
                pos = p; break;                                     /* one word longer than the line: hard break */
            }
            if (c == ' ') { last_space = p; have_space = 1; }
            cells++;
            p += hv_cell_len(c);
        }
    }
    if (hv.line_count == 0) HV_LINES[hv.line_count++] = 0;
    if (hv.scroll >= hv.line_count) hv.scroll = hv.line_count - 1;
}

/* End offset (exclusive) of wrapped line `i`, excluding its terminating newline. */
static inline u32 hv_line_end(u32 i) {
    u32 e = (i + 1 < hv.line_count) ? HV_LINES[i + 1] : hv.text_len;
    if (e > hv.text_len) e = hv.text_len;
    if (e > HV_LINES[i] && HV_TEXT[e - 1] == '\n') return e - 1;
    /* a line that was WRAPPED (rather than ended by a newline) ends just before the space the break
     * fell on -- hv_layout() swallowed that space; don't draw it */
    if (i + 1 < hv.line_count && e > HV_LINES[i] && HV_TEXT[e - 1] == ' ') e--;
    return e;
}

/* The link (index into HV_LINKS) covering text offset `off`, or -1. */
static inline int hv_link_at(u32 off) {
    for (u32 k = 0; k < hv.link_count; k++) {
        if (off >= HV_LINKS[k].tstart && off < HV_LINKS[k].tend) return (int)k;
        if (HV_LINKS[k].tstart > off) break;      /* links are recorded in text order */
    }
    return -1;
}

/* Text offset of column `col` on wrapped line `line` (clamped to the line's end), or -1 if past it. */
static inline int hv_offset_at(u32 line, u32 col) {
    if (line >= hv.line_count) return -1;
    u32 p = HV_LINES[line], e = hv_line_end(line), cells = 0;
    while (p < e) {
        if (cells == col) return (int)p;
        p += hv_cell_len(HV_TEXT[p]);
        cells++;
    }
    return -1;
}

#endif
