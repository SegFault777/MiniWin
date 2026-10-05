/* Fuzzer for the HTML engine: random tag soup + random CSS through dom_parse -> rd_layout -> rd_paint, under ASan/UBSan.
 * The OS has no memory protection, so an out-of-bounds write in the renderer is not a "segfault", it is a corrupted
 * kernel. Anything this finds is worth fixing.   usage: host_rd_fuzz [iterations] [seed] */
#include "rd_host.h"
static unsigned int *fb; static int fbw = 320, fbh = 240;
#define RD_PUTPIXEL(x, y, c) (fb[(y) * fbw + (x)] = (c))
#define RD_FILLRECT(x, y, w, h, c) do { for (int _j = (y); _j < (y) + (h); _j++) for (int _i = (x); _i < (x) + (w); _i++) fb[_j * fbw + _i] = (c); } while (0)
#define RD_COLOR(rgb) ((unsigned int)(rgb))
#include "../../kernel/render.h"

static unsigned long long rng_state;
static unsigned rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17; return (unsigned)(rng_state >> 11); }
static const char *tags[] = { "div","p","span","a href=x","b","i","ul","ol","li","table","tr","td","th","thead","tbody","caption","h1","h2","pre","blockquote",
  "form","input type=text size=9999","input type=checkbox","select","option","textarea cols=99999 rows=99999","button","img width=99999 height=99999 alt=x","img","svg","iframe","details","summary",
  "center","font size=+9 color=#f0f","nav","section","dl","dt","dd","br","hr","small","sub","sup","u","s","code","label","fieldset","legend","video","object","td colspan=99999","td rowspan=0","td colspan=0 rowspan=99999",
  "table width=1","div style='display:flex;flex-wrap:wrap'","div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(1px,1fr))'","div style='display:inline-block;width:99999px'",
  "div style='display:table'","div style='display:table-cell'","div style='float:left;width:50%'","div style='width:calc(100% - 99999px)'","div style='margin:-99999px'","div style='padding:99999px'",
  "div style='font-size:9999px'","div style='font-size:0'","div style='line-height:0'","div style='position:absolute;left:-9999px'","div style='white-space:pre'","div style='text-indent:-9999px'" };
static const char *css[] = { "*{margin:0;padding:0}","div>p+span~a:first-child{color:red}","a[href^=x]:not(.q){display:block}","li:nth-child(2n+1){display:inline}","@media (max-width:100px){p{display:none}}",
  "td:nth-child(-n+3){width:1px}",":root{--x:#fff}p{color:var(--x,var(--y))}","div{display:flex;gap:99999px}","div{display:grid}","table{border-collapse:collapse}","p{margin:calc(1px + 2em - 3%)}",
  "span{padding:min(1px,2px) max(3px,4px)}","h1{font:bold 99px/99 serif}","p{width:clamp(1px,50%,99999px)}",".a{border:99px solid red}","@supports (x:y){p{color:blue}}","}{;;{{","div{background:linear-gradient(red,blue)}" };

int main(int argc, char **argv) {
    int iters = argc > 1 ? atoi(argv[1]) : 500;
    rng_state = argc > 2 ? strtoull(argv[2], 0, 10) : 88172645463325252ull;
    fb = calloc((size_t)fbw * fbh, 4);
    static u8 buf[1 << 18];
    for (int it = 0; it < iters; it++) {
        u32 o = 0;
        #define EMIT(s) do { const char *_s = (s); while (*_s && o < sizeof(buf) - 400) buf[o++] = (u8)*_s++; } while (0)
        if (rnd() % 3) { EMIT("<style>"); int n = rnd() % 6; for (int k = 0; k < n; k++) EMIT(css[rnd() % (sizeof(css) / sizeof(css[0]))]); EMIT("</style>"); }
        int n = 5 + rnd() % 120, depth = 0;
        for (int k = 0; k < n; k++) {
            unsigned r = rnd() % 10;
            if (r < 5) { const char *t = tags[rnd() % (sizeof(tags) / sizeof(tags[0]))]; EMIT("<"); EMIT(t); if (rnd() % 4 == 0) EMIT(" class=a id=b"); EMIT(">"); depth++; }
            else if (r < 7) { if (depth > 0 && rnd() % 2) { EMIT("</"); EMIT(tags[rnd() % 20]); EMIT(">"); depth--; } else EMIT("text 한글 &amp; &#169; "); }
            else if (r < 8) { int w = rnd() % 200; for (int q = 0; q < w; q++) buf[o < sizeof(buf) - 4 ? o++ : o] = (u8)('a' + rnd() % 26 + (rnd() % 7 == 0 ? -64 : 0)); EMIT(" "); }
            else if (r < 9) EMIT("<br>");
            else EMIT("<!-- c --><![CDATA[x]]>&bogus; & <");
        }
        int vw = 40 + rnd() % 700, vh = 100 + rnd() % 400;
        dom_parse(buf, o); L.pool_mark = dom_pool_len; rd_after_parse();
        rd_relayout(vw, vh, 1);
        int sc = (int)(rnd() % (unsigned)(L.doc_h + 50));
        rd_paint(5, 5, imin(vw, fbw - 10), imin(vh, fbh - 10), sc);
        u32 u; rd_hit((int)(rnd() % 400), (int)(rnd() % 400), &u);
        if (rnd() % 7 == 0) { for (u32 nn = 1; nn < dom_node_count; nn++) if (RD_NODES[nn].tag == TG_INPUT || RD_NODES[nn].tag == TG_TEXTAREA) { rd_ctl_focus(nn); rd_ctl_key('x'); rd_ctl_key('\b'); break; } char q[64]; rd_form_query(1, 0, q, sizeof q); }
    }
    printf("fuzz ok: %d iterations\n", iters);
    return 0;
}
