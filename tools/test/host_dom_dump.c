/* Dumps the DOM kernel/dom.h builds for an HTML file: a quick way to eyeball the tree builder. */
#include "rd_host.h"
#include "../../kernel/dom.h"

static void dump(u32 n, int depth) {
    rd_node_t *e = &RD_NODES[n];
    for (int i = 0; i < depth; i++) printf("  ");
    if (e->kind == RDK_TEXT) {
        printf("\"");
        for (u32 k = 0; k < e->b && k < 60; k++) { u8 c = RD_POOL[e->a + k]; if (c == '\n') printf("\\n"); else if (c < 0x20) printf("^%c", c + 64); else putchar(c); }
        printf("\"\n");
        return;
    }
    if (e->kind == RDK_DOC) printf("#document\n"); else {
        printf("<%s", rd_tag_names[e->tag]);
        for (u32 i = 0; i < e->b; i++) {
            rd_attr_t *a = &RD_ATTRS[e->a + i];
            printf(" %s=\"%.*s\"", rd_attr_names[a->id], a->vlen > 40 ? 40 : a->vlen, RD_POOL + a->voff);
        }
        printf(">\n");
    }
    for (u32 c = e->first; c; c = RD_NODES[c].next) dump(c, depth + 1);
}

int main(int argc, char **argv) {
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : stdin;
    static u8 buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    dom_parse(buf, (u32)n);
    dump(0, 0);
    char t[128]; dom_title(t, sizeof(t));
    printf("title=[%s] nodes=%u attrs=%u pool=%u trunc=%d\n", t, dom_node_count, dom_attr_count, dom_pool_len, dom_truncated);
    return 0;
}
