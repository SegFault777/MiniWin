/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_GFX_H
#define MW_GFX_H

/* mw_gfx.h -- drawing helpers built from the syscall table's put_pixel / fill_rect / draw_string (SDK 2.0). All coordinates are in the
 * drawing area (sys.screen_w x sys.screen_h: the window's client area, or the screen). */

static inline u32 mw_rgb(int r, int g, int b) { return ((u32)(r & 255) << 16) | ((u32)(g & 255) << 8) | (u32)(b & 255); }
/* 0..256 blend of two colours: t = 0 gives a, t = 256 gives b */
static inline u32 mw_mix(u32 a, u32 b, int t) {
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255, br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return mw_rgb(ar + (br - ar) * t / 256, ag + (bg - ag) * t / 256, ab + (bb - ab) * t / 256);
}
static inline void mw_hline(int x, int y, int w, u32 c) { sys.fill_rect(x, y, w, 1, c); }
static inline void mw_vline(int x, int y, int h, u32 c) { sys.fill_rect(x, y, 1, h, c); }
/* a straight line (Bresenham) */
static inline void mw_line(int x0, int y0, int x1, int y1, u32 c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0, sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        sys.put_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
/* a circle outline / a filled circle (midpoint algorithm) */
static inline void mw_circle(int cx, int cy, int r, u32 c) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        sys.put_pixel(cx + x, cy + y, c); sys.put_pixel(cx - x, cy + y, c); sys.put_pixel(cx + x, cy - y, c); sys.put_pixel(cx - x, cy - y, c);
        sys.put_pixel(cx + y, cy + x, c); sys.put_pixel(cx - y, cy + x, c); sys.put_pixel(cx + y, cy - x, c); sys.put_pixel(cx - y, cy - x, c);
        y++;
        if (err < 0) err += 2 * y + 1; else { x--; err += 2 * (y - x) + 1; }
    }
}
static inline void mw_fill_circle(int cx, int cy, int r, u32 c) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        sys.fill_rect(cx - x, cy + y, 2 * x + 1, 1, c); sys.fill_rect(cx - x, cy - y, 2 * x + 1, 1, c);
        sys.fill_rect(cx - y, cy + x, 2 * y + 1, 1, c); sys.fill_rect(cx - y, cy - x, 2 * y + 1, 1, c);
        y++;
        if (err < 0) err += 2 * y + 1; else { x--; err += 2 * (y - x) + 1; }
    }
}
/* a vertical gradient from colour a (top) to b (bottom) */
static inline void mw_gradient_v(int x, int y, int w, int h, u32 a, u32 b) {
    for (int j = 0; j < h; j++) sys.fill_rect(x, y + j, w, 1, mw_mix(a, b, h > 1 ? j * 256 / (h - 1) : 0));
}
/* an ARGB bitmap (w x h pixels, 0xAARRGGBB). The syscall table cannot read the screen back, so there is no blending: a pixel is drawn
 * when its alpha is 128 or more, skipped otherwise. */
static inline void mw_blit_argb(int x, int y, int w, int h, const u32 *px) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) { u32 p = px[j * w + i]; if ((p >> 24) >= 128) sys.put_pixel(x + i, y + j, p & 0xFFFFFF); }
}
/* text aligned inside a box of width w */
static inline void mw_text_center(int x, int y, int w, const char *s, u32 c) { sys.draw_string_ko(x + (w - sys.text_width(s)) / 2, y, s, c); }
static inline void mw_text_right(int x, int y, int w, const char *s, u32 c)  { sys.draw_string_ko(x + w - sys.text_width(s), y, s, c); }
/* printf-style text: mw_printf(10, 20, MW_BLACK, "%d items", n) -- up to 127 characters */
static inline void mw_printf(int x, int y, u32 c, const char *fmt, ...) {
    char buf[128]; __builtin_va_list ap; __builtin_va_start(ap, fmt);
    mw_vsnprintf(buf, sizeof buf, fmt, ap);
    __builtin_va_end(ap);
    sys.draw_string_ko(x, y, buf, c);
}
/* text wrapped to a width, breaking at spaces; returns the y after the last line. Lines are MW_FONT_H + 2 apart. */
static inline int mw_text_wrap(int x, int y, int w, const char *s, u32 c) {
    int per = w / MW_FONT_W; if (per < 1) per = 1;
    while (*s) {
        int n = 0, brk = -1;
        while (s[n] && s[n] != '\n' && n < per) { if (s[n] == ' ') brk = n; n++; }
        if (s[n] && s[n] != '\n' && brk > 0) n = brk;
        char line[96]; int k = n < 95 ? n : 95; mw_memcpy(line, s, (u32)k); line[k] = 0;
        sys.draw_string_ko(x, y, line, c); y += MW_FONT_H + 2;
        s += n; if (*s == ' ' || *s == '\n') s++;
    }
    return y;
}

#endif
