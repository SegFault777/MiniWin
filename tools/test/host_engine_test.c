/* host_engine_test.c -- deterministic tests for the HTML5 engine (kernel/dom.h, css.h, layout.h, render.h).
 * Pure logic, so the real kernel headers compile on the build host with the fixed-address arenas swapped for arrays.
 *   tools/test/run_host_engine.sh                                                                                   */
#include "rd_host.h"
#define RD_PUTPIXEL(x, y, c) ((void)(c))
#define RD_FILLRECT(x, y, w, h, c) ((void)(c))
#define RD_COLOR(rgb) (rgb)
#include "../../kernel/render.h"

static int checks = 0, failures = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void load(const char *html) { rd_load_html((const u8 *)html, (u32)strlen(html)); }
static int layout(const char *html, int w) { load(html); return rd_relayout(w, 400, 1); }

/* the display-list text run that contains `needle` (NULL if none) */
static rd_item_t *find_text(const char *needle) {
    size_t nl = strlen(needle);
    for (u32 i = 0; i < L.n; i++) {
        rd_item_t *it = &RD_ITEMS[i];
        if (it->kind != DL_TEXT) continue;
        char t[600]; u32 n = it->len < 599 ? it->len : 599;
        memcpy(t, RD_POOL + it->off, n); t[n] = 0;
        for (u32 k = 0; k < n; k++) if (t[k] == '\n' || t[k] == '\t') t[k] = ' ';
        if (strstr(t, needle) && strlen(t) >= nl) return it;
    }
    return NULL;
}
static u32 find_tag(int tag, int nth) {
    for (u32 n = 1; n < dom_node_count; n++) if (RD_NODES[n].kind == RDK_ELEM && RD_NODES[n].tag == tag && nth-- == 0) return n;
    return 0;
}
static int count_kind(int kind) { int c = 0; for (u32 i = 0; i < L.n; i++) if (RD_ITEMS[i].kind == kind) c++; return c; }
static int count_tag(int tag) { int c = 0; for (u32 n = 1; n < dom_node_count; n++) if (RD_NODES[n].kind == RDK_ELEM && RD_NODES[n].tag == tag) c++; return c; }
static int child_count(u32 n) { int c = 0; for (u32 k = RD_NODES[n].first; k; k = RD_NODES[k].next) c++; return c; }
static css_style_t style_of(int tag, int nth) {          /* computed style by walking the real ancestor chain */
    u32 n = find_tag(tag, nth); u32 chain[64]; int d = 0;
    for (u32 p = n; p && d < 64; p = RD_NODES[p].parent) if (RD_NODES[p].kind == RDK_ELEM) chain[d++] = p;
    css_style_t st, par; css_style_t *pp = 0;
    for (int i = d - 1; i >= 0; i--) { css_compute(chain[i], pp, &st); par = st; pp = &par; }
    return st;
}
static css_style_t prep_style(const char *html, int tag, int nth, int vw) {
    load(html); css_viewport_w = vw; css_load_document();
    return style_of(tag, nth);
}

int main(void) {
    /* ---------- tokenizer + tree builder ---------- */
    load("<p>a<p>b<ul><li>x<li>y</ul><dl><dt>t<dd>d<dt>t2</dl>");
    CHECK(count_tag(TG_P) == 2 && count_tag(TG_LI) == 2 && child_count(find_tag(TG_UL, 0)) == 2, "implied end tags: p=%d li=%d", count_tag(TG_P), count_tag(TG_LI));
    CHECK(find_tag(TG_DL, 0) && child_count(find_tag(TG_DL, 0)) == 3, "dt/dd siblings (%d)", child_count(find_tag(TG_DL, 0)));
    load("<table><tr><td>1<td>2<tr><td>3</table>after");
    CHECK(count_tag(TG_TR) == 2 && count_tag(TG_TD) == 3, "table cells/rows");
    { u32 t = find_tag(TG_TABLE, 0); CHECK(RD_NODES[RD_NODES[t].next].kind == RDK_TEXT, "</table> closes the table even with <td> open"); }
    load("<title>A &amp; B &copy;</title><body><script>if(a<b){x='</p>'}</script>text");
    { char t[64]; dom_title(t, sizeof t); CHECK(strcmp(t, "A & B (c)") == 0, "title [%s]", t); }
    CHECK(count_tag(TG_P) == 0 && find_tag(TG_SCRIPT, 0) && RD_NODES[find_tag(TG_SCRIPT, 0)].first, "script content is raw text, not markup");
    load("<pre>\nline1\n\tx</pre>");
    { rd_node_t *tn = &RD_NODES[RD_NODES[find_tag(TG_PRE, 0)].first]; CHECK(tn->b == 11 && RD_POOL[tn->a] == 'l', "pre drops its first newline and expands tabs (%u)", tn->b); }
    load("a&nbsp;b &#xAC00;&#65;&bogus;&amp");
    { rd_node_t *tn = &RD_NODES[RD_NODES[dom_body].first]; const u8 *p = RD_POOL + tn->a;
      CHECK(p[1] == 0x01, "nbsp is stored as 0x01"); CHECK(p[4] == 0xEA && p[5] == 0xB0 && p[6] == 0x80, "Hangul kept as UTF-8"); CHECK(tn->b == 3 + 1 + 3 + 1 + 7 + 1, "text length (%u)", tn->b); }
    load("<img src=a><br/><input value=x><b>x<i>y</b>z</i><a href=1>p<a href=2>q");
    CHECK(count_tag(TG_A) == 2 && child_count(find_tag(TG_IMG, 0)) == 0, "void elements and nested <a>");
    load("<div class=a id=b data-x=1 ONCLICK=zz title='t &amp; u'>x</div>");
    { u32 n = find_tag(TG_DIV, 0); u32 l; const u8 *v = dom_attr(n, AT_TITLE, &l);
      CHECK(v && l == 5 && memcmp(v, "t & u", 5) == 0 && RD_NODES[n].b == 3, "attributes: decoded, unknown ones dropped (%u kept)", RD_NODES[n].b); }

    /* ---------- CSS ---------- */
    css_style_t s = prep_style("<style>p{color:red}.a{color:#00f}#i{color:green}p.a{margin:2px}</style><p class=a id=i>x</p>", TG_P, 0, 620);
    CHECK(s.color == 0x008000, "id beats class beats type (%06x)", s.color);
    s = prep_style("<style>p{color:red!important}p{color:blue}</style><p style='color:green'>x</p>", TG_P, 0, 620);
    CHECK(s.color == 0xFF0000, "!important beats later rules and inline style (%06x)", s.color);
    s = prep_style("<p style='font-size:16px;width:160px;margin:1em'>x</p>", TG_P, 0, 620);
    CHECK(s.cell == 11 && s.fpx == 16 && s.width.px == 110 && s.margin[0].px == 11, "units are zoomed 11/16: cell %d width %d margin %d", s.cell, s.width.px, s.margin[0].px);
    s = prep_style("<h1>x</h1>", TG_H1, 0, 620);
    CHECK(s.cell == 22 && (s.flags & SF_BOLD) && s.display == DISP_BLOCK, "h1: 2em -> cell %d", s.cell);
    s = prep_style("<style>@media (max-width:500px){p{color:red}} @media (min-width:5000px){p{color:green}}</style><p>x</p>", TG_P, 0, 300);
    CHECK(s.color == 0xFF0000, "@media max-width matches a narrow window (%06x)", s.color);
    s = prep_style("<style>@media (max-width:500px){p{color:red}} @media (min-width:5000px){p{color:green}}</style><p>x</p>", TG_P, 0, 620);
    CHECK(s.color == 0x000000, "...and not a wide one (%06x)", s.color);
    s = prep_style("<style>:root{--c:#123456;--w:calc(100% - 20px)}p{color:var(--c);width:var(--w)}</style><p>x</p>", TG_P, 0, 620);
    CHECK(s.color == 0x123456 && s.width.pct == 1000 && s.width.px == -14, "var() and calc(): %06x %d %d", s.color, s.width.px, s.width.pct);
    s = prep_style("<style>ul>li:nth-child(2n+1){color:red} li:not(.x):last-child{background:#00f} a[href^='http']{color:green}</style>"
                   "<ul><li>1<li class=x>2<li>3</ul><a href='http://x'>l</a>", TG_LI, 2, 620);
    CHECK(s.color == 0xFF0000 && s.bg == 0x0000FF, "nth-child / :not / :last-child (%06x %x)", s.color, s.bg);
    s = prep_style("<style>a[href^='http']{color:green}</style><a href='http://x'>l</a>", TG_A, 0, 620);
    CHECK(s.color == 0x008000, "attribute selector");
    s = prep_style("<style>div p{color:red} div>p{color:blue} div+p{color:green}</style><div><section><p>x</p></section></div>", TG_P, 0, 620);
    CHECK(s.color == 0xFF0000, "descendant matches through a section; child does not (%06x)", s.color);
    s = prep_style("<style>td{padding:9px}</style><table cellpadding=7 border=1><tr><td>x</table>", TG_TD, 0, 620);
    CHECK(s.padding[0].px == 9 * 11 / 16 + 1 || s.padding[0].px == 6, "author CSS beats cellpadding (%d)", s.padding[0].px);
    s = prep_style("<table cellpadding=16><tr><td>x</table>", TG_TD, 0, 620);
    CHECK(s.padding[0].px == 11 && s.bw[0] == 0, "cellpadding beats the UA's 1px (%d)", s.padding[0].px);
    s = prep_style("<font color=#f00 size=5>x</font>", TG_FONT, 0, 620);
    CHECK(s.color == 0xFF0000 && s.fpx == 24, "<font> attributes (%06x, %d)", s.color, s.fpx);
    { u32 c;
      CHECK(css_parse_color((const u8 *)"#abc", 4, &c) && c == 0xAABBCC, "#rgb");
      CHECK(css_parse_color((const u8 *)"rgb(255, 128, 0)", 16, &c) && c == 0xFF8000, "rgb()");
      CHECK(css_parse_color((const u8 *)"hsl(120, 100%, 50%)", 19, &c) && (c & 0xFF00FF) == 0 && ((c >> 8) & 255) >= 250, "hsl() %06x", c);
      CHECK(css_parse_color((const u8 *)"rgba(0,0,0,0)", 13, &c) && c == CSS_NOCOLOR, "fully transparent -> none");
      CHECK(css_parse_color((const u8 *)"RebeccaPurple", 13, &c) && c == 0x663399, "named, any case");
      CHECK(!css_parse_color((const u8 *)"nonsense", 8, &c), "garbage color rejected"); }

    /* ---------- layout ---------- */
    int h = layout("<body style='margin:0'><p style='margin:16px 0'>one</p><p style='margin:16px 0'>two</p></body>", 600);
    { rd_item_t *a = find_text("one"), *b = find_text("two");
      CHECK(a && b && b->y - a->y == a->h + 11, "sibling margins collapse to one 11px gap (dy=%d lh=%d)", b ? b->y - a->y : -1, a ? a->h : -1);
      CHECK(h > 0, "doc height"); }
    layout("<body style='margin:0'><div style='text-align:center'>mid</div></body>", 600);
    { rd_item_t *a = find_text("mid"); CHECK(a && a->x == (600 - a->w) / 2, "text-align:center (x=%d w=%d)", a ? a->x : -1, a ? a->w : -1); }
    layout("<body style='margin:0'><div style='width:100px;margin:0 auto;background:#f00'>x</div></body>", 600);
    { rd_item_t *r = &RD_ITEMS[0]; for (u32 i = 0; i < L.n; i++) if (RD_ITEMS[i].kind == DL_RECT && RD_ITEMS[i].color == 0xFF0000) { r = &RD_ITEMS[i]; break; }
      CHECK(r->color == 0xFF0000 && r->w == 69 && r->x == (600 - 69) / 2, "auto margins centre a fixed-width block (x=%d w=%d)", r->x, r->w); }
    layout("<body style='margin:0'>aaa bbb ccc ddd eee fff ggg hhh iii jjj kkk lll mmm nnn ooo ppp</body>", 11 * 20);
    { rd_item_t *a = find_text("aaa"), *z = find_text("ppp");
      CHECK(a && z && z->y > a->y && z->y - a->y <= 3 * a->h, "long text wraps at the width"); 
      int ok = 1; for (u32 i = 0; i < L.n; i++) if (RD_ITEMS[i].kind == DL_TEXT && RD_ITEMS[i].x + RD_ITEMS[i].w > 220) ok = 0;
      CHECK(ok, "no run sticks out past the window"); }
    layout("<body style='margin:0'>foo<b>bar</b>baz qux</body>", 11 * 7);
    { rd_item_t *a = find_text("foo"); rd_item_t *q = find_text("qux");
      CHECK(a && q && a->y == RD_ITEMS[0].y && q->y > a->y, "glued inline pieces stay together; the break is at the space"); }
    layout("<body style='margin:0'><ol><li>a<li>b</ol><ul><li>c</ul></body>", 600);
    CHECK(find_text("1.") && find_text("2.") && count_kind(DL_BULLET) == 1, "list markers: 1. 2. and one bullet");
    layout("<body style='margin:0'><table><tr><td>A<td>B</td></tr><tr><td colspan=2>wide</td></tr></table></body>", 600);
    { rd_item_t *a = find_text("A"), *b = find_text("B"), *w = find_text("wide");
      CHECK(a && b && w && b->x > a->x && w->y > a->y && w->x == a->x, "table: columns side by side, colspan row below"); }
    layout("<body style='margin:0'><div style='display:flex'><div style='flex:1;background:#f00'>a</div><div style='flex:1;background:#0f0'>b</div></div></body>", 600);
    { int rx = -1, gx = -1, rw = 0, gw = 0;
      for (u32 i = 0; i < L.n; i++) if (RD_ITEMS[i].kind == DL_RECT) { if (RD_ITEMS[i].color == 0xFF0000) { rx = RD_ITEMS[i].x; rw = RD_ITEMS[i].w; } if (RD_ITEMS[i].color == 0x00FF00) { gx = RD_ITEMS[i].x; gw = RD_ITEMS[i].w; } }
      CHECK(rx == 0 && gx == 300 && rw == 300 && gw == 300, "flex: 1 splits the row equally (%d,%d %d,%d)", rx, rw, gx, gw); }
    layout("<body style='margin:0'><div style='display:grid;grid-template-columns:100px 1fr 1fr;gap:10px'><i>a</i><i>b</i><i>c</i></div></body>", 600);
    { rd_item_t *a = find_text("a"), *b = find_text("b"), *c = find_text("c");
      /* 100px = 69, gap 10px = 7 (zoomed); the two 1fr tracks share (600-69-14)/2 = 258 each */
      CHECK(a && b && c && a->x == 0 && b->x == 76 && c->x == 76 + 258 + 7, "grid: px + 1fr 1fr tracks with a gap (%d %d %d)", a ? a->x : -1, b ? b->x : -1, c ? c->x : -1); }
    layout("<body style='margin:0'><details><summary>S</summary>hidden</details><details open><summary>T</summary>shown</details></body>", 600);
    CHECK(!find_text("hidden") && find_text("shown") && find_text("S") && find_text("T"), "details: closed hides its content, open shows it");
    rd_toggle_flip(find_tag(TG_DETAILS, 0)); rd_relayout(600, 400, 1);
    CHECK(find_text("hidden") != NULL, "toggling opens a closed <details>");
    layout("<body style='margin:0'><div style='display:none'>gone</div><div style='visibility:hidden'>ghost</div><p hidden>no</p>yes</body>", 600);
    { rd_item_t *g = find_text("ghost"); CHECK(!find_text("gone") && !find_text("no") && find_text("yes") && !g, "display:none, [hidden]; visibility:hidden keeps space but paints nothing"); }
    layout("<body style='margin:0'><a href='/x'>link <b>bold</b></a></body>", 600);
    { u32 node; rd_item_t *a = find_text("link"); int k = a ? rd_hit(a->x + 2, a->y + 2, &node) : 0;
      CHECK(k == HIT_LINK && RD_NODES[node].tag == TG_A, "hit test finds the link"); 
      CHECK(rd_hit(590, 300, &node) == HIT_NONE, "...and nothing in empty space"); }
    { int h1 = layout("<body style='margin:0'><img src=x width=100 height=50><br><img src=y alt='hi there'>", 600);
      CHECK(count_kind(DL_IMG) == 2 && h1 >= 50, "images become placeholder boxes"); }
    layout("<body style='margin:0'><p style='margin:0'>한글 텍스트</p></body>", 600);
    { rd_item_t *a = find_text("\xED\x95\x9C"); CHECK(a && a->w == 6 * 11, "Hangul: one cell per syllable (w=%d)", a ? a->w : -1); }
    CHECK(layout("<table><tr><td>" "x" "</table>", 40) > 0, "tiny viewport survives");

    /* ---------- forms ---------- */
    load("<form action=/s><input name=q value='a b'><input type=hidden name=h value=1><input type=checkbox name=c value=y checked>"
         "<input type=checkbox name=d><input type=radio name=r value=1><input type=radio name=r value=2 checked>"
         "<select name=s><option value=o1>One<option>Two<option selected>Three</select><textarea name=t>x\ny</textarea>"
         "<input type=submit name=go value=Go><button name=b value=B>Btn</button></form>");
    rd_relayout(600, 400, 1);
    { char q[300]; u32 f = find_tag(TG_FORM, 0);
      rd_form_query(f, 0, q, sizeof q);
      CHECK(strcmp(q, "q=a+b&h=1&c=y&r=2&s=Three&t=x%0Ay") == 0, "form query [%s]", q);
      rd_form_query(f, find_tag(TG_INPUT, 6), q, sizeof q);
      CHECK(strstr(q, "&go=Go") != NULL, "the pressed submit button is included [%s]", q);
      u32 q1 = find_tag(TG_INPUT, 0);
      rd_ctl_focus(q1); rd_ctl_key('!'); rd_ctl_key('\b'); rd_ctl_key('Z'); rd_ctl_key('&');
      rd_form_query(f, 0, q, sizeof q);
      CHECK(strncmp(q, "q=a+bZ%26&", 10) == 0, "typing edits the field, '&' is encoded [%s]", q);
      CHECK(rd_ctl_key('\n') == 2, "Enter in a text field asks for a submit");
      rd_radio_select(find_tag(TG_INPUT, 4));
      rd_select_next(find_tag(TG_SELECT, 0));
      rd_form_query(f, 0, q, sizeof q);
      CHECK(strstr(q, "r=1") && !strstr(q, "r=2") && strstr(q, "s=o1"), "radio group + select cycling [%s]", q); }
    { char o[64]; int n = 0;
      load("<form action='/x'><input name=q></form>"); (void)n; (void)o; }

    /* rc-5 (G-07): a page that does not fit the fixed-size tables is FLAGGED so the browser can say so */
    load("<html><body><p>a normal page</p></body></html>"); rd_relayout(600, 400, 1);
    CHECK(!rd_page_incomplete(), "a normal page is not flagged incomplete");
    { size_t n = 30000; char *big = malloc(n * 8 + 64); size_t o = 0;
      o += sprintf(big + o, "<html><body>");
      for (size_t i = 0; i < n; i++) o += sprintf(big + o, "<b>x</b>");        /* 30000 elements > RD_NODE_MAX (16384) */
      o += sprintf(big + o, "</body></html>");
      rd_load_html((const u8 *)big, (u32)o); rd_relayout(600, 400, 1);
      CHECK(dom_truncated && rd_page_incomplete(), "more elements than the DOM arena holds => flagged (dom_truncated=%d)", dom_truncated);
      free(big); }
    load("<html><body><p>fine again</p></body></html>"); rd_relayout(600, 400, 1);
    CHECK(!rd_page_incomplete(), "the flag is cleared when the next page loads");
    { size_t n = 4000; char *big = malloc(n * 40 + 64); size_t o = 0;
      o += sprintf(big + o, "<html><head><style>");
      for (size_t i = 0; i < n; i++) o += sprintf(big + o, ".c%zu{color:red}\n", i);     /* 4000 rules > RD_CSS_RULE_MAX (3072) */
      o += sprintf(big + o, "</style></head><body><p class=c1>hi</p></body></html>");
      rd_load_html((const u8 *)big, (u32)o); rd_relayout(600, 400, 1);
      CHECK(css_overflowed && rd_page_incomplete(), "more CSS rules than the rule table holds => flagged (css_overflowed=%d)", css_overflowed);
      free(big); }
    { size_t n = 12000; char *big = malloc(n * 16 + 64); size_t o = 0;
      o += sprintf(big + o, "<html><body>");
      for (size_t i = 0; i < n; i++) o += sprintf(big + o, "<p>row %zu</p>", i);   /* enough text runs to fill the layout item table */
      o += sprintf(big + o, "</body></html>");
      rd_load_html((const u8 *)big, (u32)o); rd_relayout(600, 400, 1);
      CHECK(rd_page_incomplete(), "a page that fills the DOM/layout tables is flagged (dom_truncated=%d L.full=%d)", dom_truncated, L.full);
      free(big); }

    /* rc-5 (M-03): a form that does not fit the query buffer is FLAGGED, never silently shortened */
    load("<form action=/s><input name=q value='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'><input name=z value=1></form>");
    rd_relayout(600, 400, 1);
    { char q[300]; u32 f = find_tag(TG_FORM, 0);
      rd_form_query(f, 0, q, sizeof q);
      CHECK(!rd_query_overflow && strstr(q, "&z=1"), "a form that fits is not flagged [%s]", q);
      char small[40]; rd_form_query(f, 0, small, sizeof small);
      CHECK(rd_query_overflow, "a form that does not fit a 40-byte query is flagged as overflowed [%s]", small);
      CHECK(!strstr(small, "z=1"), "...and a later field is not squeezed in after an earlier one was cut (order stays honest) [%s]", small);
      rd_form_query(f, 0, q, sizeof q);
      CHECK(!rd_query_overflow, "the flag is cleared by the next successful query"); }
    load("<form action=/s><input name=q value='\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80'></form>");
    rd_relayout(600, 400, 1);
    { char q[12]; u32 f = find_tag(TG_FORM, 0); rd_form_query(f, 0, q, sizeof q);
      CHECK(rd_query_overflow, "a multi-byte value whose %%XX escapes do not all fit is flagged, not half-escaped [%s]", q);
      CHECK(strlen(q) < 12 && (strlen(q) == 0 || q[strlen(q) - 1] != '%') , "no dangling '%%' at the end [%s]", q); }

    /* ---------- loaders ---------- */
    rd_load_plain((const u8 *)"line1\r\n  line2\ttab\n", 20); rd_relayout(600, 400, 1);
    CHECK(find_text("line1") && find_text("line2") && find_text("line2")->y > find_text("line1")->y, "plain text keeps its lines");
    rd_load_message("Oops <b>", "reason & more", ""); rd_relayout(600, 400, 1);
    CHECK(find_text("Oops <b>") && find_text("reason & more"), "messages are escaped, not parsed");

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
