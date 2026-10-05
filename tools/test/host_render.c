/* Renders an HTML file through the real engine (dom.h/css.h/layout.h/render.h) into a PPM image:
 *   host_render page.html out.ppm [width] [height]
 * The screen is `width` x 480 by default; the page is laid out in a (width-20) wide window like the OS's web window. */
#include "rd_host.h"
static unsigned int *fb; static int fbw, fbh;
#define RD_PUTPIXEL(x, y, c) (fb[(y) * fbw + (x)] = (c))
#define RD_FILLRECT(x, y, w, h, c) do { for (int _j = (y); _j < (y) + (h); _j++) for (int _i = (x); _i < (x) + (w); _i++) fb[_j * fbw + _i] = (c); } while (0)
#define RD_COLOR(rgb) ((unsigned int)(rgb))
#include "../../kernel/render.h"

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: host_render in.html out.ppm [w] [h] [scroll] [plain]\n"); return 1; }
    int W = argc > 3 ? atoi(argv[3]) : 640, H = argc > 4 ? atoi(argv[4]) : 480, scroll = argc > 5 ? atoi(argv[5]) : 0;
    int plain = argc > 6;
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror("open"); return 1; }
    static u8 buf[1 << 20]; size_t n = fread(buf, 1, sizeof(buf), f); fclose(f);
    if (plain) rd_load_plain(buf, (u32)n); else rd_load_html(buf, (u32)n);
    fbw = W; fbh = H; fb = calloc((size_t)W * H, 4);
    for (int i = 0; i < W * H; i++) fb[i] = 0xC0C0C0;
    int vw = W - 20, vh = H - 20;
    int dh = rd_relayout(vw, vh, 1);
    fprintf(stderr, "doc_h=%d items=%u full=%d nodes=%u rules=%u pool=%u\n", dh, L.n, L.full, dom_node_count, css_rule_count, dom_pool_len);
    rd_paint(10, 10, vw, vh, scroll);
    FILE *o = fopen(argv[2], "wb");
    fprintf(o, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) { unsigned c = fb[i]; fputc((c >> 16) & 255, o); fputc((c >> 8) & 255, o); fputc(c & 255, o); }
    fclose(o);
    return 0;
}
