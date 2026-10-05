#ifndef DOM_H
#define DOM_H
#include "io.h"

/* ============================================================
 * dom.h -- the front half of MiniWeb's HTML5 engine: bytes in, a
 * tree of nodes out.
 *
 *   1. TOKENIZER: walks the HTML byte stream the way the HTML5 spec's
 *      tokenizer does -- tags, attributes (double-quoted, single-quoted
 *      and the unquoted kind that sloppy sites love), comments,
 *      doctypes, character references (&amp; &#169; &#xA9;), and the
 *      "raw text" elements (<script>, <style>, <title>, <textarea>...)
 *      whose contents are NOT markup no matter how much they look like it.
 *   2. TREE BUILDER: a deliberately *simplified* HTML5 tree construction.
 *      It knows the void elements (<br>, <img>...), supplies the
 *      implied <html>/<head>/<body>, closes a <p> when a block shows up,
 *      closes <li>/<dd>/<dt>/<option>/<tr>/<td> when their next sibling
 *      starts, and resolves end tags with the spec's "scope" idea so
 *      a stray </div> can't tear down a table it doesn't own. What it
 *      does NOT do: the adoption-agency algorithm (misnested <b><i></b>
 *      is repaired by the simple rule, not the 40-step one), foster
 *      parenting (text dumped inside <table> stays inside it), or
 *      scripting. Real pages survive this; the HTML5 conformance suite
 *      would weep.
 *
 * MEMORY: no malloc here, same as everywhere in this kernel. Nodes,
 * attributes and text live in fixed arenas (memmap.h gives kernel
 * builds a slab of high RAM; host tests supply plain arrays by
 * defining RD_NODES etc. before including this file). When an arena
 * fills up, parsing stops cleanly and `dom_truncated` is set -- the
 * half-built tree is still a valid tree.
 *
 * TEXT: stored as bytes in the pool, already reduced to what the
 * bitmap fonts can draw: ASCII as itself, Hangul as its 3-byte UTF-8
 * form, everything else mapped to an ASCII look-alike (curly quotes
 * -> straight, e-acute -> e, the euro sign -> E ...) or '?'. So every
 * byte >= 0xE0 in the pool starts a 3-byte Hangul cell, and "length in
 * cells" is a trivial scan. U+00A0 (nbsp) is stored as 0x01: a space
 * that does not collapse and does not break.
 * ============================================================ */

#ifndef RD_NODES
#include "memmap.h"
#define RD_NODE_MAX   MW_RD_NODE_MAX
#define RD_ATTR_MAX   MW_RD_ATTR_MAX
#define RD_POOL_SIZE  MW_RD_POOL_SIZE
#define RD_NODES ((rd_node_t *)MW_RD_NODES_ADDR)
#define RD_ATTRS ((rd_attr_t *)MW_RD_ATTRS_ADDR)
#define RD_POOL  ((u8 *)MW_RD_POOL_ADDR)
#endif

/* ---- tags: one X-macro list so the enum and the name table can never drift apart ---- */
#define RD_TAG_LIST(X) \
 X(A,"a") X(ABBR,"abbr") X(ADDRESS,"address") X(AREA,"area") X(ARTICLE,"article") X(ASIDE,"aside") X(AUDIO,"audio") \
 X(B,"b") X(BASE,"base") X(BDI,"bdi") X(BDO,"bdo") X(BIG,"big") X(BLOCKQUOTE,"blockquote") X(BODY,"body") X(BR,"br") X(BUTTON,"button") \
 X(CANVAS,"canvas") X(CAPTION,"caption") X(CENTER,"center") X(CITE,"cite") X(CODE,"code") X(COL,"col") X(COLGROUP,"colgroup") \
 X(DATA,"data") X(DATALIST,"datalist") X(DD,"dd") X(DEL,"del") X(DETAILS,"details") X(DFN,"dfn") X(DIALOG,"dialog") X(DIR,"dir") X(DIV,"div") X(DL,"dl") X(DT,"dt") \
 X(EM,"em") X(EMBED,"embed") X(FIELDSET,"fieldset") X(FIGCAPTION,"figcaption") X(FIGURE,"figure") X(FONT,"font") X(FOOTER,"footer") X(FORM,"form") X(FRAME,"frame") X(FRAMESET,"frameset") \
 X(H1,"h1") X(H2,"h2") X(H3,"h3") X(H4,"h4") X(H5,"h5") X(H6,"h6") X(HEAD,"head") X(HEADER,"header") X(HGROUP,"hgroup") X(HR,"hr") X(HTML,"html") \
 X(I,"i") X(IFRAME,"iframe") X(IMG,"img") X(INPUT,"input") X(INS,"ins") X(KBD,"kbd") X(LABEL,"label") X(LEGEND,"legend") X(LI,"li") X(LINK,"link") X(LISTING,"listing") \
 X(MAIN,"main") X(MAP,"map") X(MARK,"mark") X(MARQUEE,"marquee") X(MATH,"math") X(MENU,"menu") X(META,"meta") X(METER,"meter") \
 X(NAV,"nav") X(NOBR,"nobr") X(NOEMBED,"noembed") X(NOFRAMES,"noframes") X(NOSCRIPT,"noscript") \
 X(OBJECT,"object") X(OL,"ol") X(OPTGROUP,"optgroup") X(OPTION,"option") X(OUTPUT,"output") \
 X(P,"p") X(PARAM,"param") X(PICTURE,"picture") X(PLAINTEXT,"plaintext") X(PRE,"pre") X(PROGRESS,"progress") \
 X(Q,"q") X(RP,"rp") X(RT,"rt") X(RUBY,"ruby") X(S,"s") X(SAMP,"samp") X(SCRIPT,"script") X(SEARCH,"search") X(SECTION,"section") X(SELECT,"select") \
 X(SLOT,"slot") X(SMALL,"small") X(SOURCE,"source") X(SPAN,"span") X(STRIKE,"strike") X(STRONG,"strong") X(STYLE,"style") X(SUB,"sub") X(SUMMARY,"summary") X(SUP,"sup") X(SVG,"svg") \
 X(TABLE,"table") X(TBODY,"tbody") X(TD,"td") X(TEMPLATE,"template") X(TEXTAREA,"textarea") X(TFOOT,"tfoot") X(TH,"th") X(THEAD,"thead") X(TIME,"time") X(TITLE,"title") X(TR,"tr") X(TRACK,"track") X(TT,"tt") \
 X(U,"u") X(UL,"ul") X(VAR,"var") X(VIDEO,"video") X(WBR,"wbr") X(XMP,"xmp")

enum {
    TG_NONE = 0,
    TG_UNKNOWN,                       /* <my-widget>, <path>, <g>, ... : parsed fine, styled as a plain inline */
#define X(id, nm) TG_##id,
    RD_TAG_LIST(X)
#undef X
    TG__COUNT
};

static const char *const rd_tag_names[TG__COUNT] = {
    0, "?",
#define X(id, nm) nm,
    RD_TAG_LIST(X)
#undef X
};

/* ---- attributes: only the ones the engine acts on are kept (data-*, aria-*, onclick=... are dropped on
 * the floor, which also keeps 200KB of inline-JSON-in-attributes from eating the pool) ---- */
#define RD_ATTR_LIST(X) \
 X(ID,"id") X(CLASS,"class") X(HREF,"href") X(SRC,"src") X(ALT,"alt") X(STYLE,"style") X(TYPE,"type") X(VALUE,"value") \
 X(NAME,"name") X(COLSPAN,"colspan") X(ROWSPAN,"rowspan") X(WIDTH,"width") X(HEIGHT,"height") X(ALIGN,"align") X(VALIGN,"valign") \
 X(BGCOLOR,"bgcolor") X(COLOR,"color") X(SIZE,"size") X(COLS,"cols") X(ROWS,"rows") X(BORDER,"border") X(CELLPADDING,"cellpadding") \
 X(CELLSPACING,"cellspacing") X(PLACEHOLDER,"placeholder") X(CHECKED,"checked") X(SELECTED,"selected") X(DISABLED,"disabled") \
 X(HIDDEN,"hidden") X(OPEN,"open") X(TARGET,"target") X(TITLE,"title") X(ACTION,"action") X(METHOD,"method") X(FOR,"for") \
 X(FACE,"face") X(NOWRAP,"nowrap") X(TEXT,"text") X(LINK,"link") X(MAXLENGTH,"maxlength") X(READONLY,"readonly") X(REL,"rel") \
 X(MEDIA,"media") X(START,"start") X(BACKGROUND,"background") X(HSPACE,"hspace") X(VSPACE,"vspace") X(ROLE,"role") X(LANG,"lang")

enum {
    AT_NONE = 0,
#define X(id, nm) AT_##id,
    RD_ATTR_LIST(X)
#undef X
    AT__COUNT
};
static const char *const rd_attr_names[AT__COUNT] = {
    0,
#define X(id, nm) nm,
    RD_ATTR_LIST(X)
#undef X
};

typedef struct { u8 id; u8 pad; u16 vlen; u32 voff; } rd_attr_t;      /* 8 bytes */

#define RDK_NONE 0
#define RDK_DOC  1
#define RDK_ELEM 2
#define RDK_TEXT 3

/* node 0 is the document; 0 also means "no node" in every link field */
typedef struct {
    u32 parent, first, last, next;
    u32 a;      /* text: pool offset of its bytes      element: index of its first attribute */
    u32 b;      /* text: byte length                   element: attribute count */
    u8  kind, tag;
    u16 flags;
    u32 aux;    /* the layout engine's scratch (cached intrinsic widths) */
} rd_node_t;

#define DOMF_NONE 0

/* ---- the document ---- */
static u32 dom_node_count;      /* includes node 0 */
static u32 dom_attr_count;
static u32 dom_pool_len;
static int dom_truncated;
static u32 dom_html, dom_head, dom_body;

#define DOM_STACK_MAX 192
static u32 dom_stk[DOM_STACK_MAX];
static u32 dom_sp;
static int dom_in_head;
static u32 dom_txt_node;        /* text node currently being extended (0 = start a new one on the next byte) */
static int dom_skip_lf;         /* swallow one leading newline (right after <pre>/<textarea>/<listing>) */
static int dom_foreign;         /* inside <svg>/<math>: "/>" really does self-close there */

static inline int dom_is_ws(u8 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static inline u8  dom_lc(u8 c) { return (c >= 'A' && c <= 'Z') ? (u8)(c + 32) : c; }
static inline int dom_is_alpha(u8 c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static inline int dom_is_alnum(u8 c) { return dom_is_alpha(c) || (c >= '0' && c <= '9'); }

/* ---- tag classification ---- */
static inline int dom_tag_is_void(u32 t) {
    switch (t) {
        case TG_AREA: case TG_BASE: case TG_BR: case TG_COL: case TG_EMBED: case TG_HR: case TG_IMG: case TG_INPUT:
        case TG_LINK: case TG_META: case TG_PARAM: case TG_SOURCE: case TG_TRACK: case TG_WBR: case TG_FRAME:
            return 1;
        default: return 0;
    }
}
/* "special" elements in the spec's sense: the ones an unmatched end tag must not reach across */
static inline int dom_tag_is_special(u32 t) {
    switch (t) {
        case TG_ADDRESS: case TG_ARTICLE: case TG_ASIDE: case TG_BLOCKQUOTE: case TG_BODY: case TG_BR: case TG_BUTTON:
        case TG_CAPTION: case TG_CENTER: case TG_COL: case TG_COLGROUP: case TG_DD: case TG_DETAILS: case TG_DIR: case TG_DIV:
        case TG_DL: case TG_DT: case TG_EMBED: case TG_FIELDSET: case TG_FIGCAPTION: case TG_FIGURE: case TG_FOOTER:
        case TG_FORM: case TG_H1: case TG_H2: case TG_H3: case TG_H4: case TG_H5: case TG_H6: case TG_HEAD: case TG_HEADER:
        case TG_HGROUP: case TG_HR: case TG_HTML: case TG_IFRAME: case TG_IMG: case TG_INPUT: case TG_LI: case TG_LINK:
        case TG_LISTING: case TG_MAIN: case TG_MARQUEE: case TG_MENU: case TG_META: case TG_NAV: case TG_NOEMBED:
        case TG_NOFRAMES: case TG_NOSCRIPT: case TG_OBJECT: case TG_OL: case TG_P: case TG_PARAM: case TG_PLAINTEXT:
        case TG_PRE: case TG_SCRIPT: case TG_SEARCH: case TG_SECTION: case TG_SELECT: case TG_SOURCE: case TG_STYLE:
        case TG_SUMMARY: case TG_TABLE: case TG_TBODY: case TG_TD: case TG_TEMPLATE: case TG_TEXTAREA: case TG_TFOOT:
        case TG_TH: case TG_THEAD: case TG_TITLE: case TG_TR: case TG_TRACK: case TG_UL: case TG_WBR: case TG_XMP:
            return 1;
        default: return 0;
    }
}
static inline int dom_tag_is_heading(u32 t) { return t >= TG_H1 && t <= TG_H6; }
/* opening one of these closes a <p> that is still open */
static inline int dom_tag_closes_p(u32 t) {
    switch (t) {
        case TG_ADDRESS: case TG_ARTICLE: case TG_ASIDE: case TG_BLOCKQUOTE: case TG_CENTER: case TG_DETAILS:
        case TG_DIALOG: case TG_DIR: case TG_DIV: case TG_DL: case TG_FIELDSET: case TG_FIGCAPTION: case TG_FIGURE:
        case TG_FOOTER: case TG_FORM: case TG_H1: case TG_H2: case TG_H3: case TG_H4: case TG_H5: case TG_H6:
        case TG_HEADER: case TG_HGROUP: case TG_HR: case TG_LI: case TG_DD: case TG_DT: case TG_LISTING: case TG_MAIN:
        case TG_MENU: case TG_NAV: case TG_OL: case TG_P: case TG_PLAINTEXT: case TG_PRE: case TG_SEARCH:
        case TG_SECTION: case TG_SUMMARY: case TG_TABLE: case TG_UL: case TG_XMP:
            return 1;
        default: return 0;
    }
}
static inline int dom_tag_is_headish(u32 t) {
    switch (t) {
        case TG_TITLE: case TG_META: case TG_LINK: case TG_STYLE: case TG_SCRIPT: case TG_BASE: case TG_NOSCRIPT: case TG_TEMPLATE:
            return 1;
        default: return 0;
    }
}
/* raw-text elements: 1 = RAWTEXT (no entities), 2 = RCDATA (entities decoded) */
static inline int dom_tag_rawness(u32 t) {
    switch (t) {
        case TG_SCRIPT: case TG_STYLE: case TG_XMP: case TG_IFRAME: case TG_NOEMBED: case TG_NOFRAMES: return 1;
        case TG_TITLE: case TG_TEXTAREA: return 2;
        default: return 0;
    }
}

static inline u32 dom_tag_id(const u8 *name, u32 n) {
    if (n == 0 || n > 12) return TG_UNKNOWN;
    for (u32 t = TG_A; t < TG__COUNT; t++) {
        const char *s = rd_tag_names[t];
        u32 i = 0;
        while (i < n && s[i] && (u8)s[i] == name[i]) i++;
        if (i == n && s[i] == 0) return t;
    }
    return TG_UNKNOWN;
}
static inline u32 dom_attr_id(const u8 *name, u32 n) {
    if (n == 0 || n > 12) return AT_NONE;
    for (u32 t = 1; t < AT__COUNT; t++) {
        const char *s = rd_attr_names[t];
        u32 i = 0;
        while (i < n && s[i] && (u8)s[i] == name[i]) i++;
        if (i == n && s[i] == 0) return t;
    }
    return AT_NONE;
}

/* ---- attribute lookup (used by the CSS matcher and the layout engine) ---- */
static inline int dom_attr_find(u32 node, u32 id) {
    rd_node_t *n = &RD_NODES[node];
    if (n->kind != RDK_ELEM) return -1;
    for (u32 i = 0; i < n->b; i++) if (RD_ATTRS[n->a + i].id == id) return (int)(n->a + i);
    return -1;
}
static inline const u8 *dom_attr(u32 node, u32 id, u32 *len) {
    int i = dom_attr_find(node, id);
    if (i < 0) { if (len) *len = 0; return 0; }
    if (len) *len = RD_ATTRS[i].vlen;
    return RD_POOL + RD_ATTRS[i].voff;
}
static inline int dom_has_attr(u32 node, u32 id) { return dom_attr_find(node, id) >= 0; }

/* ============================================================
 * characters
 * ============================================================ */
static inline void dom_pool_putc(u8 c) {
    if (dom_pool_len + 8 < RD_POOL_SIZE) RD_POOL[dom_pool_len++] = c; else dom_truncated = 1;
}

static inline int dom_is_hangul(u32 cp) {
    return (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F);
}

/* Maps one Unicode code point to the bytes stored in the pool; returns the count (0..4). */
static inline u32 dom_map_cp(u32 cp, u8 *out) {
    static const char latin1[] =   /* U+00C0..U+00FF reduced to the nearest ASCII letter */
        "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    const char *s = 0;
    if (cp < 0x80) { out[0] = (u8)cp; return 1; }
    if (cp == 0xA0) { out[0] = 0x01; return 1; }
    if (dom_is_hangul(cp)) {
        out[0] = (u8)(0xE0 | (cp >> 12)); out[1] = (u8)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (u8)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp == 0xD7) { out[0] = 'x'; return 1; }
    if (cp >= 0xC0 && cp <= 0xFF) { out[0] = (u8)latin1[cp - 0xC0]; return 1; }
    switch (cp) {
        case 0x2013: case 0x2014: case 0x2212: case 0x2010: case 0x2011: case 0x2012: s = "-"; break;
        case 0x2018: case 0x2019: case 0x2032: case 0x201A: case 0x2039: case 0x203A: s = "'"; break;
        case 0x201C: case 0x201D: case 0x2033: case 0x201E: s = "\""; break;
        case 0x2022: case 0x25CF: case 0x25AA: case 0x25E6: case 0x2605: case 0x2606: case 0x2043: s = "*"; break;
        case 0x2026: s = "..."; break;
        case 0xB7: case 0x2027: case 0x22C5: s = "."; break;
        case 0xA9: s = "(c)"; break;      case 0xAE: s = "(R)"; break;      case 0x2122: s = "TM"; break;
        case 0xAB: s = "<<"; break;       case 0xBB: s = ">>"; break;
        case 0x2190: s = "<-"; break;     case 0x2192: s = "->"; break;     case 0x2191: s = "^"; break;
        case 0x2193: s = "v"; break;      case 0x2194: s = "<->"; break;
        case 0x25B6: case 0x25BA: s = ">"; break;
        case 0x25C0: case 0x25C4: s = "<"; break;
        case 0x25B2: case 0x2303: s = "^"; break;
        case 0x25BC: case 0x25BE: case 0x2304: s = "v"; break;
        case 0x2713: case 0x2714: s = "v"; break;      case 0x2715: case 0x2717: case 0x2718: s = "x"; break;
        case 0x2665: case 0x2764: s = "<3"; break;
        case 0xB0: s = "o"; break;        case 0xB1: s = "+/-"; break;
        case 0xA3: s = "L"; break;        case 0xA5: s = "Y"; break;        case 0x20AC: s = "E"; break;
        case 0xA2: s = "c"; break;        case 0xA7: s = "S"; break;        case 0xB6: s = "P"; break;
        case 0xBD: s = "1/2"; break;      case 0xBC: s = "1/4"; break;      case 0xBE: s = "3/4"; break;
        case 0xB2: s = "2"; break;        case 0xB3: s = "3"; break;        case 0xB9: s = "1"; break;
        case 0xB5: s = "u"; break;        case 0xA1: s = "!"; break;        case 0xBF: s = "?"; break;
        case 0x2009: case 0x2002: case 0x2003: case 0x2005: case 0x2006: case 0x202F: s = " "; break;
        case 0xAD: case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: case 0x200E: case 0x200F: return 0;
        default: s = "?"; break;
    }
    u32 n = 0;
    while (s[n] && n < 4) { out[n] = (u8)s[n]; n++; }
    return n;
}

/* named character references, the ones real pages actually use */
typedef struct { const char *name; u16 cp; } dom_ent_t;
static const dom_ent_t dom_ents[] = {
    {"amp",'&'},{"lt",'<'},{"gt",'>'},{"quot",'"'},{"apos",'\''},{"nbsp",0xA0},{"copy",0xA9},{"reg",0xAE},
    {"trade",0x2122},{"laquo",0xAB},{"raquo",0xBB},{"middot",0xB7},{"hellip",0x2026},{"mdash",0x2014},{"ndash",0x2013},
    {"lsquo",0x2018},{"rsquo",0x2019},{"ldquo",0x201C},{"rdquo",0x201D},{"sbquo",0x201A},{"bdquo",0x201E},{"bull",0x2022},
    {"times",0xD7},{"divide",0xF7},{"deg",0xB0},{"plusmn",0xB1},{"larr",0x2190},{"rarr",0x2192},{"uarr",0x2191},
    {"darr",0x2193},{"harr",0x2194},{"shy",0xAD},{"zwnj",0x200C},{"zwj",0x200D},{"ensp",0x2002},{"emsp",0x2003},
    {"thinsp",0x2009},{"euro",0x20AC},{"pound",0xA3},{"yen",0xA5},{"cent",0xA2},{"sect",0xA7},{"para",0xB6},
    {"frac12",0xBD},{"frac14",0xBC},{"frac34",0xBE},{"sup2",0xB2},{"sup3",0xB3},{"sup1",0xB9},{"micro",0xB5},
    {"iexcl",0xA1},{"iquest",0xBF},{"hearts",0x2665},{"check",0x2713},{"star",0x2606},{"starf",0x2605},
    {"lsaquo",0x2039},{"rsaquo",0x203A},{"prime",0x2032},{"Prime",0x2033},{"minus",0x2212},{"infin",0x221E},
    {"ne",0x2260},{"le",0x2264},{"ge",0x2265},{"asymp",0x2248},{"permil",0x2030},{"dagger",0x2020},{"Dagger",0x2021},
    {"agrave",0xE0},{"aacute",0xE1},{"acirc",0xE2},{"atilde",0xE3},{"auml",0xE4},{"aring",0xE5},{"ccedil",0xE7},
    {"egrave",0xE8},{"eacute",0xE9},{"ecirc",0xEA},{"euml",0xEB},{"igrave",0xEC},{"iacute",0xED},{"icirc",0xEE},
    {"iuml",0xEF},{"ntilde",0xF1},{"ograve",0xF2},{"oacute",0xF3},{"ocirc",0xF4},{"otilde",0xF5},{"ouml",0xF6},
    {"ugrave",0xF9},{"uacute",0xFA},{"ucirc",0xFB},{"uuml",0xFC},{"yacute",0xFD},{"szlig",0xDF},
    {"Agrave",0xC0},{"Aacute",0xC1},{"Acirc",0xC2},{"Atilde",0xC3},{"Auml",0xC4},{"Aring",0xC5},{"Ccedil",0xC7},
    {"Egrave",0xC8},{"Eacute",0xC9},{"Ecirc",0xCA},{"Euml",0xCB},{"Iacute",0xCD},{"Ntilde",0xD1},{"Oacute",0xD3},
    {"Ouml",0xD6},{"Uacute",0xDA},{"Uuml",0xDC},
};

static inline int dom_name_eq(const u8 *s, u32 n, const char *lit) {
    u32 i = 0;
    for (; lit[i]; i++) if (i >= n || s[i] != (u8)lit[i]) return 0;
    return i == n;
}

/* Decodes a character reference starting just AFTER the '&' at s[0..avail). Returns the bytes consumed
 * (including the ';' when there is one), or 0 if this '&' is just an ampersand. `in_attr` is the spec's
 * attribute-value rule: a legacy no-semicolon reference followed by '=' or a letter is left alone. */
static inline u32 dom_entity(const u8 *s, u32 avail, u32 *cp_out, int in_attr) {
    if (avail == 0) return 0;
    if (s[0] == '#') {
        u32 i = 1, v = 0; int hex = 0, nd = 0;
        if (i < avail && (s[i] == 'x' || s[i] == 'X')) { hex = 1; i++; }
        while (i < avail && nd < 8) {
            u8 c = s[i]; u32 d;
            if (c >= '0' && c <= '9') d = (u32)(c - '0');
            else if (hex && c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
            else if (hex && c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
            else break;
            v = v * (hex ? 16u : 10u) + d; i++; nd++;
        }
        if (nd == 0) return 0;
        if (i < avail && s[i] == ';') i++;
        if (v == 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) v = 0xFFFD;
        *cp_out = v; return i;
    }
    u32 n = 0;
    while (n < avail && n < 10 && dom_is_alnum(s[n])) n++;
    if (n == 0) return 0;
    int semi = (n < avail && s[n] == ';');
    for (u32 k = 0; k < sizeof(dom_ents) / sizeof(dom_ents[0]); k++) {
        if (dom_name_eq(s, n, dom_ents[k].name)) {
            if (!semi) {
                /* legacy references that may omit the semicolon */
                int legacy = dom_ents[k].cp == '&' || dom_ents[k].cp == '<' || dom_ents[k].cp == '>' || dom_ents[k].cp == '"' ||
                             dom_ents[k].cp == 0xA0 || dom_ents[k].cp == 0xA9 || dom_ents[k].cp == 0xAE;
                if (!legacy) return 0;
                if (in_attr && n < avail && (s[n] == '=')) return 0;
            }
            *cp_out = dom_ents[k].cp; return n + (semi ? 1 : 0);
        }
    }
    return 0;
}

/* Decodes one UTF-8 sequence at p[0..avail); returns bytes consumed (>= 1). Bad bytes become U+FFFD one at a time. */
static inline u32 dom_utf8(const u8 *p, u32 avail, u32 *cp) {
    u8 c = p[0];
    if (c < 0x80) { *cp = c; return 1; }
    u32 need = (c >= 0xF0 && c < 0xF8) ? 4 : (c >= 0xE0 && c < 0xF0) ? 3 : (c >= 0xC2 && c < 0xE0) ? 2 : 0;
    if (need == 0 || need > avail) { *cp = 0xFFFD; return 1; }
    u32 v = need == 2 ? (u32)(c & 0x1F) : need == 3 ? (u32)(c & 0x0F) : (u32)(c & 0x07);
    for (u32 i = 1; i < need; i++) {
        if ((p[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (u32)(p[i] & 0x3F);
    }
    *cp = v; return need;
}

/* ============================================================
 * nodes
 * ============================================================ */
static inline u32 dom_new_node(u8 kind, u8 tag) {
    if (dom_node_count >= RD_NODE_MAX) { dom_truncated = 1; return 0; }
    u32 i = dom_node_count++;
    rd_node_t *n = &RD_NODES[i];
    n->parent = n->first = n->last = n->next = 0; n->a = n->b = 0;
    n->kind = kind; n->tag = tag; n->flags = 0; n->aux = 0;
    return i;
}
static inline void dom_append(u32 parent, u32 child) {
    rd_node_t *p = &RD_NODES[parent], *c = &RD_NODES[child];
    c->parent = parent; c->next = 0;
    if (p->last) RD_NODES[p->last].next = child; else p->first = child;
    p->last = child;
}
static inline u32 dom_cur(void) { return dom_sp ? dom_stk[dom_sp - 1] : 0; }
static inline u32 dom_cur_tag(void) { return dom_sp ? RD_NODES[dom_stk[dom_sp - 1]].tag : 0; }

static inline void dom_reset(void) {
    dom_node_count = 0; dom_attr_count = 0; dom_pool_len = 0; dom_truncated = 0;
    dom_sp = 0; dom_in_head = 1; dom_txt_node = 0; dom_skip_lf = 0; dom_foreign = 0;
    dom_new_node(RDK_DOC, 0);
    dom_html = dom_new_node(RDK_ELEM, TG_HTML); dom_append(0, dom_html);
    dom_head = dom_new_node(RDK_ELEM, TG_HEAD); dom_append(dom_html, dom_head);
    dom_body = dom_new_node(RDK_ELEM, TG_BODY); dom_append(dom_html, dom_body);
    dom_stk[dom_sp++] = dom_html;
    dom_stk[dom_sp++] = dom_head;
}

static inline void dom_enter_body(void) {
    if (!dom_in_head) return;
    dom_in_head = 0;
    dom_sp = 0;
    dom_stk[dom_sp++] = dom_html;
    dom_stk[dom_sp++] = dom_body;
}

static inline int dom_in_pre(void) {
    for (u32 i = dom_sp; i > 0; i--) {
        u32 t = RD_NODES[dom_stk[i - 1]].tag;
        if (t == TG_PRE || t == TG_XMP || t == TG_TEXTAREA || t == TG_LISTING || t == TG_PLAINTEXT) return 1;
    }
    return 0;
}

/* The text sink: bytes go to the end of the pool and onto the current text node, which is created on the
 * first byte and is extended in place for as long as nothing else has been written to the pool in between. */
static inline void dom_text_byte(u8 c) {
    if (dom_pool_len + 8 >= RD_POOL_SIZE) { dom_truncated = 1; return; }
    if (dom_txt_node == 0) {
        u32 par = dom_cur();
        u32 last = RD_NODES[par].last;
        if (last && RD_NODES[last].kind == RDK_TEXT && RD_NODES[last].a + RD_NODES[last].b == dom_pool_len) {
            dom_txt_node = last;
        } else {
            u32 n = dom_new_node(RDK_TEXT, 0);
            if (!n) return;
            RD_NODES[n].a = dom_pool_len;
            dom_append(par, n);
            dom_txt_node = n;
        }
    }
    RD_POOL[dom_pool_len++] = c;
    RD_NODES[dom_txt_node].b++;
}

/* Appends h[0..n) as text to the current element. `decode` = resolve character references. */
static inline void dom_add_text(const u8 *h, u32 n, int decode) {
    u32 i = 0;
    if (dom_in_head && dom_cur_tag() == TG_HEAD) {      /* whitespace between head elements is nobody's business */
        int any = 0;
        for (u32 k = 0; k < n; k++) if (!dom_is_ws(h[k])) { any = 1; break; }
        if (!any) return;
        dom_enter_body();
    }
    if (dom_skip_lf) {
        dom_skip_lf = 0;
        if (n > 0 && h[0] == '\n') i = 1;
        else if (n > 1 && h[0] == '\r' && h[1] == '\n') i = 2;
    }
    int pre = dom_in_pre();
    while (i < n && !dom_truncated) {
        u32 cp, used;
        u8 c = h[i];
        if (c == '&' && decode) {
            u32 e = dom_entity(h + i + 1, n - i - 1, &cp, 0);
            if (e) { i += 1 + e; goto emit; }
        }
        if (c == '\r') { i++; if (i < n && h[i] == '\n') i++; cp = '\n'; goto emit; }
        used = dom_utf8(h + i, n - i, &cp);
        i += used;
    emit:
        if (cp == '\t' && pre) { for (int k = 0; k < 4; k++) dom_text_byte(' '); continue; }
        if (cp == '\f' || cp == '\t' || cp == '\n') { dom_text_byte(cp == '\f' ? ' ' : (u8)cp); continue; }
        if (cp < 0x20 || cp == 0x7F) continue;
        u8 out[4];
        u32 k = dom_map_cp(cp, out);
        for (u32 j = 0; j < k; j++) dom_text_byte(out[j]);
    }
}

/* ============================================================
 * the open-element stack: scope searches
 * ============================================================ */
#define SCOPE_GENERIC 0
#define SCOPE_BUTTON  1
#define SCOPE_LI      2
#define SCOPE_TABLE   3
static inline int dom_scope_stop(u32 t, int kind) {
    switch (kind) {
        case SCOPE_TABLE: return t == TG_HTML || t == TG_TABLE || t == TG_TEMPLATE;
        case SCOPE_BUTTON: if (t == TG_BUTTON) return 1; /* fallthrough */
        case SCOPE_LI: if (kind == SCOPE_LI && (t == TG_OL || t == TG_UL)) return 1; /* fallthrough */
        default:
            return t == TG_HTML || t == TG_TABLE || t == TG_TD || t == TG_TH || t == TG_CAPTION || t == TG_MARQUEE ||
                   t == TG_OBJECT || t == TG_TEMPLATE;
    }
}
/* returns the stack index of the nearest open `tag` before a scope boundary, or -1 */
static inline int dom_find(u32 tag, int kind) {
    for (int i = (int)dom_sp - 1; i >= 0; i--) {
        u32 t = RD_NODES[dom_stk[i]].tag;
        if (t == tag) return i;
        if (dom_scope_stop(t, kind)) return -1;
    }
    return -1;
}
static inline void dom_pop_to(int idx) { if (idx >= 0 && (u32)idx < dom_sp) dom_sp = (u32)idx; }

static inline u32 dom_insert_elem(u32 tag, u32 afirst, u32 acount, int push) {
    u32 n = dom_new_node(RDK_ELEM, (u8)tag);
    if (!n) return 0;
    RD_NODES[n].a = afirst; RD_NODES[n].b = acount;
    dom_append(dom_cur(), n);
    if (push && dom_sp < DOM_STACK_MAX) dom_stk[dom_sp++] = n;
    return n;
}

/* ============================================================
 * tree construction: one start tag / one end tag at a time
 * ============================================================ */
static inline void dom_close_p(void) { int i = dom_find(TG_P, SCOPE_BUTTON); if (i >= 0) dom_pop_to(i); }

/* `afirst/acount`: this tag's attributes, already sitting at the end of RD_ATTRS; `pool_mark`: where the pool
 * stood before the first of their values was written (so a dropped tag can give both back). */
static inline u32 dom_start(u32 tag, u32 afirst, u32 acount, u32 pool_mark, int selfclose) {
    dom_txt_node = 0;
    if (tag == TG_HTML) {
        if (RD_NODES[dom_html].b == 0 && acount && afirst + acount == dom_attr_count) { RD_NODES[dom_html].a = afirst; RD_NODES[dom_html].b = acount; }
        else { dom_attr_count = afirst; dom_pool_len = pool_mark; }
        return 0;
    }
    if (tag == TG_HEAD) { dom_attr_count = afirst; dom_pool_len = pool_mark; return 0; }
    if (tag == TG_BODY) {
        dom_enter_body();
        if (RD_NODES[dom_body].b == 0 && acount && afirst + acount == dom_attr_count) { RD_NODES[dom_body].a = afirst; RD_NODES[dom_body].b = acount; }
        else { dom_attr_count = afirst; dom_pool_len = pool_mark; }
        return 0;
    }
    if (tag == TG_FRAME || tag == TG_FRAMESET) { dom_attr_count = afirst; dom_pool_len = pool_mark; return 0; }
    if (dom_in_head && !dom_tag_is_headish(tag)) dom_enter_body();

    if (!dom_in_head) {
        if (dom_tag_closes_p(tag)) dom_close_p();
        if (dom_tag_is_heading(tag) && dom_tag_is_heading(dom_cur_tag())) dom_sp--;
        switch (tag) {
            case TG_LI:
                for (int i = (int)dom_sp - 1; i >= 0; i--) {
                    u32 t = RD_NODES[dom_stk[i]].tag;
                    if (t == TG_LI) { dom_pop_to(i); break; }
                    if (dom_tag_is_special(t) && t != TG_ADDRESS && t != TG_DIV && t != TG_P) break;
                }
                break;
            case TG_DD: case TG_DT:
                for (int i = (int)dom_sp - 1; i >= 0; i--) {
                    u32 t = RD_NODES[dom_stk[i]].tag;
                    if (t == TG_DD || t == TG_DT) { dom_pop_to(i); break; }
                    if (dom_tag_is_special(t) && t != TG_ADDRESS && t != TG_DIV && t != TG_P) break;
                }
                break;
            case TG_OPTION: if (dom_cur_tag() == TG_OPTION) dom_sp--; break;
            case TG_OPTGROUP: if (dom_cur_tag() == TG_OPTION) dom_sp--; if (dom_cur_tag() == TG_OPTGROUP) dom_sp--; break;
            case TG_A: { int i = dom_find(TG_A, SCOPE_GENERIC); if (i >= 0) dom_pop_to(i); } break;
            case TG_BUTTON: { int i = dom_find(TG_BUTTON, SCOPE_GENERIC); if (i >= 0) dom_pop_to(i); } break;
            case TG_CAPTION: case TG_COLGROUP: case TG_THEAD: case TG_TBODY: case TG_TFOOT:
                while (dom_sp > 2 && dom_cur_tag() != TG_TABLE && dom_cur_tag() != TG_TEMPLATE) dom_sp--;
                break;
            case TG_TR:
                while (dom_sp > 2) {
                    u32 t = dom_cur_tag();
                    if (t == TG_TABLE || t == TG_TBODY || t == TG_THEAD || t == TG_TFOOT || t == TG_TEMPLATE) break;
                    dom_sp--;
                }
                break;
            case TG_TD: case TG_TH: {
                while (dom_sp > 2) {
                    u32 t = dom_cur_tag();
                    if (t == TG_TR || t == TG_TABLE || t == TG_TBODY || t == TG_THEAD || t == TG_TFOOT || t == TG_TEMPLATE) break;
                    dom_sp--;
                }
                u32 t = dom_cur_tag();
                if (t == TG_TABLE || t == TG_TBODY || t == TG_THEAD || t == TG_TFOOT) dom_insert_elem(TG_TR, 0, 0, 1);   /* a <td> with no <tr>: supply one */
            } break;
            default: break;
        }
    }

    int is_void = dom_tag_is_void(tag);
    int foreign_root = (tag == TG_SVG || tag == TG_MATH);
    u32 n = dom_insert_elem(tag, afirst, acount, !(is_void || (selfclose && dom_foreign)));
    if (foreign_root && !(selfclose)) dom_foreign++;
    return n;
}

static inline void dom_end(u32 tag) {
    dom_txt_node = 0;
    switch (tag) {
        case TG_HTML: case TG_BODY: return;                       /* never closed: trailing content still belongs to the page */
        case TG_HEAD: dom_enter_body(); return;
        case TG_BR: dom_start(TG_BR, dom_attr_count, 0, dom_pool_len, 0); return;
        case TG_P: {
            int i = dom_find(TG_P, SCOPE_BUTTON);
            if (i >= 0) dom_pop_to(i);
            else if (!dom_in_head) dom_insert_elem(TG_P, 0, 0, 0);    /* "</p>" with no <p>: the spec conjures an empty one */
            return;
        }
        case TG_LI: { int i = dom_find(TG_LI, SCOPE_LI); if (i >= 0) dom_pop_to(i); return; }
        case TG_DD: case TG_DT: { int i = dom_find(tag, SCOPE_GENERIC); if (i >= 0) dom_pop_to(i); return; }
        case TG_H1: case TG_H2: case TG_H3: case TG_H4: case TG_H5: case TG_H6: {
            for (int i = (int)dom_sp - 1; i >= 0; i--) {
                u32 t = RD_NODES[dom_stk[i]].tag;
                if (dom_tag_is_heading(t)) { dom_pop_to(i); return; }
                if (dom_scope_stop(t, SCOPE_GENERIC)) return;
            }
            return;
        }
        case TG_TABLE: case TG_TD: case TG_TH: case TG_TR: case TG_TBODY: case TG_THEAD: case TG_TFOOT: case TG_CAPTION: case TG_COLGROUP: {
            int i = dom_find(tag, SCOPE_TABLE);
            if (i >= 0) dom_pop_to(i);
            return;
        }
        case TG_SVG: case TG_MATH: if (dom_foreign > 0) dom_foreign--; /* fallthrough */
        default: break;
    }
    /* block-ish end tags and formatting end tags: close the nearest matching open element in scope (and
     * everything unclosed above it); anything else: walk down, but never reach across a "special" element */
    int i = dom_find(tag, SCOPE_GENERIC);
    if (i >= 0) { dom_pop_to(i); return; }
    switch (tag) {
        case TG_A: case TG_B: case TG_BIG: case TG_CODE: case TG_EM: case TG_FONT: case TG_I: case TG_NOBR: case TG_S:
        case TG_SMALL: case TG_STRIKE: case TG_STRONG: case TG_TT: case TG_U:
            return;
        default: break;
    }
    for (int k = (int)dom_sp - 1; k >= 0; k--) {
        u32 t = RD_NODES[dom_stk[k]].tag;
        if (t == tag) { dom_pop_to(k); return; }
        if (dom_tag_is_special(t)) return;
    }
}

/* ============================================================
 * the tokenizer
 * ============================================================ */
/* case-insensitive search for "</name" at or after i; returns its offset (the '<'), or len if absent */
static inline u32 dom_find_close(const u8 *h, u32 len, u32 i, const char *name) {
    u32 nl = 0; while (name[nl]) nl++;
    for (; i + 2 + nl <= len; i++) {
        if (h[i] != '<' || h[i + 1] != '/') continue;
        u32 k = 0;
        while (k < nl && dom_lc(h[i + 2 + k]) == (u8)name[k]) k++;
        if (k != nl) continue;
        u8 after = (i + 2 + nl < len) ? h[i + 2 + nl] : '>';
        if (dom_is_ws(after) || after == '>' || after == '/') return i;
    }
    return len;
}

/* Parses attributes from h[i..len) until the tag's '>' ; writes kept attributes at the end of RD_ATTRS.
 * Returns the offset just past the '>' and reports selfclose. */
static inline u32 dom_parse_attrs(const u8 *h, u32 len, u32 i, u32 *afirst, u32 *acount, int *selfclose) {
    *afirst = dom_attr_count; *acount = 0; *selfclose = 0;
    for (;;) {
        while (i < len && (dom_is_ws(h[i]) || h[i] == '/')) {
            if (h[i] == '/' && i + 1 < len && h[i + 1] == '>') *selfclose = 1;
            i++;
        }
        if (i >= len) return len;
        if (h[i] == '>') return i + 1;
        u32 ns = i;
        while (i < len && !dom_is_ws(h[i]) && h[i] != '=' && h[i] != '>' && !(h[i] == '/' && i + 1 < len && h[i + 1] == '>')) i++;
        u8 lname[16]; u32 ln = i - ns;
        if (ln > 15) ln = 15;
        for (u32 k = 0; k < ln; k++) lname[k] = dom_lc(h[ns + k]);
        u32 aid = dom_attr_id(lname, ln);
        while (i < len && dom_is_ws(h[i])) i++;
        u32 vs = i, ve = i; int quoted = 0;
        if (i < len && h[i] == '=') {
            i++;
            while (i < len && dom_is_ws(h[i])) i++;
            if (i < len && (h[i] == '"' || h[i] == '\'')) {
                u8 q = h[i++]; vs = i;
                while (i < len && h[i] != q) i++;
                ve = i; if (i < len) i++;
                quoted = 1;
            } else {
                vs = i;
                while (i < len && !dom_is_ws(h[i]) && h[i] != '>') i++;
                ve = i;
            }
        }
        (void)quoted;
        if (aid != AT_NONE && dom_attr_count < RD_ATTR_MAX) {
            rd_attr_t *a = &RD_ATTRS[dom_attr_count];
            a->id = (u8)aid; a->pad = 0; a->voff = dom_pool_len;
            u32 start = dom_pool_len;
            for (u32 k = vs; k < ve && !dom_truncated; ) {
                u32 cp, used;
                u8 c = h[k];
                if (c == '&') { u32 e = dom_entity(h + k + 1, ve - k - 1, &cp, 1); if (e) { k += 1 + e; goto put; } }
                used = dom_utf8(h + k, ve - k, &cp); k += used;
            put:
                if (cp == '\r' || cp == '\n' || cp == '\t' || cp == '\f') cp = ' ';
                if (cp < 0x80) { if (cp >= 0x20 && cp != 0x7F) dom_pool_putc((u8)cp); }
                else { u8 o[4]; u32 m = dom_map_cp(cp, o); for (u32 j = 0; j < m; j++) dom_pool_putc(o[j]); }
            }
            u32 vl = dom_pool_len - start;
            a->vlen = (u16)(vl > 65535 ? 65535 : vl);
            dom_attr_count++; (*acount)++;
        }
    }
}

static inline void dom_parse(const u8 *h, u32 len) {
    dom_reset();
    u32 i = 0;
    while (i < len && !dom_truncated) {
        if (h[i] != '<') {
            u32 j = i;
            while (j < len && h[j] != '<') j++;
            dom_add_text(h + i, j - i, 1);
            i = j;
            continue;
        }
        /* h[i] == '<' */
        if (i + 1 >= len) { dom_add_text((const u8 *)"<", 1, 0); i++; continue; }
        u8 c1 = h[i + 1];
        if (c1 == '!') {
            if (i + 3 < len && h[i + 2] == '-' && h[i + 3] == '-') {              /* <!-- comment --> */
                u32 j = i + 4;
                if (j < len && h[j] == '>') { i = j + 1; continue; }                /* "<!-->" */
                if (j + 1 < len && h[j] == '-' && h[j + 1] == '>') { i = j + 2; continue; }
                while (j + 2 < len && !(h[j] == '-' && h[j + 1] == '-' && (h[j + 2] == '>' || (h[j + 2] == '!' && j + 3 < len && h[j + 3] == '>')))) j++;
                if (j + 2 >= len) { i = len; continue; }
                i = j + (h[j + 2] == '>' ? 3 : 4);
                continue;
            }
            u32 j = i + 2;                                                          /* doctype / CDATA / bogus comment */
            while (j < len && h[j] != '>') j++;
            i = j < len ? j + 1 : len;
            continue;
        }
        if (c1 == '?') { u32 j = i + 2; while (j < len && h[j] != '>') j++; i = j < len ? j + 1 : len; continue; }
        if (c1 == '/') {
            if (i + 2 < len && dom_is_alpha(h[i + 2])) {
                u32 j = i + 2, ns = j;
                while (j < len && !dom_is_ws(h[j]) && h[j] != '/' && h[j] != '>') j++;
                u8 nm[16]; u32 nn = j - ns; if (nn > 15) nn = 15;
                for (u32 k = 0; k < nn; k++) nm[k] = dom_lc(h[ns + k]);
                u32 tag = dom_tag_id(nm, nn);
                while (j < len && h[j] != '>') j++;                                 /* attributes on end tags are ignored */
                i = j < len ? j + 1 : len;
                dom_end(tag);
                continue;
            }
            if (i + 2 < len && h[i + 2] == '>') { i += 3; continue; }               /* "</>" */
            u32 j = i + 2; while (j < len && h[j] != '>') j++;                       /* bogus comment */
            i = j < len ? j + 1 : len;
            continue;
        }
        if (!dom_is_alpha(c1)) { dom_add_text((const u8 *)"<", 1, 0); i++; continue; }   /* a lone '<' is just text */

        /* start tag */
        u32 j = i + 1, ns = j;
        while (j < len && !dom_is_ws(h[j]) && h[j] != '/' && h[j] != '>') j++;
        u8 nm[16]; u32 nn = j - ns; if (nn > 15) nn = 15;
        for (u32 k = 0; k < nn; k++) nm[k] = dom_lc(h[ns + k]);
        u32 tag = dom_tag_id(nm, nn);
        if (tag == TG_UNKNOWN && h[ns] == 'i' && nn == 5 && nm[0] == 'i' && nm[1] == 'm' && nm[2] == 'a' && nm[3] == 'g' && nm[4] == 'e') tag = TG_IMG;
        u32 pool_mark = dom_pool_len, afirst, acount; int selfclose;
        j = dom_parse_attrs(h, len, j, &afirst, &acount, &selfclose);
        i = j;
        if (tag == TG_PRE || tag == TG_LISTING || tag == TG_TEXTAREA) dom_skip_lf = 0;
        u32 el = dom_start(tag, afirst, acount, pool_mark, selfclose);
        if (tag == TG_PRE || tag == TG_LISTING) dom_skip_lf = 1;
        int raw = dom_tag_rawness(tag);
        if (tag == TG_PLAINTEXT && el) {                                              /* everything after it is text, forever */
            dom_add_text(h + i, len - i, 0);
            i = len;
            continue;
        }
        if (raw && el && !(selfclose && dom_foreign)) {
            u32 k = dom_find_close(h, len, i, rd_tag_names[tag]);
            if (tag == TG_TEXTAREA) dom_skip_lf = 1;
            if (k > i) {
                dom_skip_lf = (tag == TG_TEXTAREA);
                if (raw == 2) dom_add_text(h + i, k - i, 1);
                else {
                    if (dom_in_head) { /* style/script in head: already there */ }
                    for (u32 q = i; q < k && !dom_truncated; q++) {                    /* raw text: bytes verbatim */
                        u8 c = h[q];
                        if (c == '\r') { if (q + 1 < k && h[q + 1] == '\n') continue; c = '\n'; }
                        dom_text_byte(c);
                    }
                }
            }
            dom_skip_lf = 0;
            u32 q = k;
            while (q < len && h[q] != '>') q++;
            i = q < len ? q + 1 : len;
            dom_txt_node = 0;
            dom_end(tag);
        }
    }
    dom_txt_node = 0;
}

/* The page's <title>, or "" -- the first title element's first text node, copied into `out` (NUL-terminated). */
static inline void dom_title(char *out, u32 outsz) {
    out[0] = 0;
    for (u32 n = 1; n < dom_node_count; n++) {
        rd_node_t *e = &RD_NODES[n];
        if (e->kind != RDK_ELEM || e->tag != TG_TITLE) continue;
        u32 c = e->first, o = 0; int sp = 0;
        while (c && o + 1 < outsz) {
            if (RD_NODES[c].kind == RDK_TEXT) {
                for (u32 k = 0; k < RD_NODES[c].b && o + 1 < outsz; k++) {
                    u8 b = RD_POOL[RD_NODES[c].a + k];
                    if (dom_is_ws(b)) { sp = 1; continue; }
                    if (sp && o) out[o++] = ' ';
                    sp = 0;
                    if (o + 1 < outsz) out[o++] = (char)(b == 0x01 ? ' ' : b);
                }
            }
            c = RD_NODES[c].next;
        }
        out[o] = 0;
        return;
    }
}

#endif
