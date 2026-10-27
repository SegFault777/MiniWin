/* SPDX-License-Identifier: GPL-3.0-only */
/* @mwp title: MiniCalc */
/* @mwp abi: 3 */
/* minicalc.c -- MiniCalc, a pocket calculator: the SDK 2.0 example that uses the toolkit (mw_ui.h), the window calls (set_size), the
 * clipboard (Copy / Paste talk to Notepad's Edit menu), sys.args, and its own icon (minicalc.png, packed into the file by the build).
 *
 * Numbers are fixed point with three decimals (1.5 is stored as 1500) -- the program has no floating point -- so the range is
 * +-2,147,483.647. Keys: 0-9 . + - * / % = (or Enter), Backspace erases the last digit, C clears, ESC quits. `run minicalc 12*3` is
 * not parsed as an expression; sys.args only supplies a first number: `run minicalc 42` opens with 42 in the display. */
#include "miniwin.h"

#define SCALE 1000
typedef struct {
    int acc, cur;                 /* the running result and the number being shown (x1000) */
    unsigned mag; int neg;        /* the number being typed: magnitude and sign */
    int dot, fd;                  /* a '.' was typed; digits typed after it (0..3) */
    char op;                      /* pending + - * / or 0 */
    int fresh;                    /* the next digit starts a new number */
    int err;
} calc_t;

static void set_cur(calc_t *c, int v) { c->cur = v; c->neg = v < 0; c->mag = (unsigned)mw_abs(v); c->dot = 0; c->fd = 0; }

static void digit(calc_t *c, int d) {
    if (c->err) return;
    if (c->fresh) { c->mag = 0; c->neg = 0; c->dot = 0; c->fd = 0; c->fresh = 0; }
    if (!c->dot) { unsigned ip = c->mag / SCALE; if (ip > 214748u) return; c->mag = (ip * 10u + (unsigned)d) * SCALE; }      /* the integer part grows by a digit */
    else if (c->fd < 3) { static const unsigned p[3] = { 100, 10, 1 }; c->mag += (unsigned)d * p[c->fd]; c->fd++; }
    c->cur = c->neg ? -(int)c->mag : (int)c->mag;
}
static int eval(calc_t *c) {
    long long r = 0; int a = c->acc, b = c->cur;
    if (c->op == '+') r = (long long)a + b;
    else if (c->op == '-') r = (long long)a - b;
    else if (c->op == '*') { int m = mw_muldiv(a, b, SCALE); r = m; }
    else if (c->op == '/') { if (b == 0) return 1; r = mw_muldiv(a, SCALE, b); }
    if (r > 2147483647LL || r < -2147483647LL) return 1;
    set_cur(c, (int)r); c->acc = c->cur;
    return 0;
}
static void press_op(calc_t *c, char o) {
    if (c->err) return;
    if (c->op && !c->fresh) { if (eval(c)) { c->err = 1; return; } } else c->acc = c->cur;
    c->op = o; c->fresh = 1;
}
static void press_eq(calc_t *c) {
    if (c->err) return;
    if (c->op) { if (eval(c)) { c->err = 1; return; } c->op = 0; }
    c->fresh = 1;
}
static void clear(calc_t *c) { mw_memset(c, 0, sizeof *c); c->fresh = 1; }
static void show(const calc_t *c, char *out, int cap) {
    if (c->err) { mw_strlcpy(out, "Error", (u32)cap); return; }
    int v = c->cur, neg = v < 0; unsigned u = (unsigned)mw_abs(v), ip = u / SCALE, fp = u % SCALE;
    if (c->dot && !c->fresh) {                       /* while typing: show exactly the digits typed, so "2." and "2.50" look as typed */
        char fr[4] = { 0, 0, 0, 0 }; int k = 0; unsigned f = fp;
        unsigned dv[3] = { 100, 10, 1 };
        for (; k < c->fd; k++) { fr[k] = (char)('0' + (f / dv[k]) % 10); }
        mw_snprintf(out, (u32)cap, "%s%u.%s", neg ? "-" : "", ip, fr);
        return;
    }
    if (fp == 0) mw_snprintf(out, (u32)cap, "%s%u", neg ? "-" : "", ip);
    else {
        char fr[4]; mw_snprintf(fr, sizeof fr, "%03u", fp);
        int n = 3; while (n > 1 && fr[n - 1] == '0') n--;
        fr[n] = 0;
        mw_snprintf(out, (u32)cap, "%s%u.%s", neg ? "-" : "", ip, fr);
    }
}
static void paste(calc_t *c) {
    char b[24]; int n = mw_clip_paste(b, sizeof b), i = 0;
    if (n <= 0) return;
    clear(c);
    if (b[0] == '-') { c->neg = 1; i = 1; }
    for (; b[i]; i++) { if (mw_isdigit(b[i])) digit(c, b[i] - '0'); else if (b[i] == '.' && !c->dot) { c->dot = 1; c->fresh = 0; } }
    c->fresh = 0;
    c->cur = c->neg ? -(int)c->mag : (int)c->mag;
}

void mwp_entry(void) {
    if (!mw_has(3)) return;                            /* needs ABI 3 (the build says so too: @mwp abi: 3) */
    const int W = 212, H = 262;
    int windowed = mw_windowed();
    if (windowed) sys.set_size(W, H);
    mw_ui_t ui; mw_ui_init(&ui);
    calc_t c; clear(&c);
    if (sys.args && sys.args[0]) { int v = mw_atoi(sys.args); if (mw_abs(v) < 2000000) { set_cur(&c, v * SCALE); c.fresh = 1; } }
    static const char *const keys[6][4] = {
        { "sqrt", "x2", "Cpy", "Pst" },
        { "C", "+/-", "%", "/" },
        { "7", "8", "9", "*" },
        { "4", "5", "6", "-" },
        { "1", "2", "3", "+" },
        { "0", ".", "<", "=" },
    };
    char text[24], status[24]; status[0] = 0;
    for (;;) {
        mw_ui_begin(&ui);
        int ox = ((int)sys.screen_w - W) / 2, oy = windowed ? 0 : ((int)sys.screen_h - H) / 2;
        if (ox < 0) ox = 0;
        mw_fill_screen(mw_ui_color(MW_TH_FACE));
        if (!windowed) sys.window_frame(ox - 1, oy - 17, W + 2, H + 19, "MiniCalc", MWI_FILES);
        show(&c, text, sizeof text);
        mw_ui_box(ox + 6, oy + 6, W - 12, 34, 1, mw_ui_color(MW_TH_FIELD));
        mw_text_right(ox + 10, oy + 16, W - 20, text, mw_ui_color(MW_TH_FIELD_TEXT));
        if (c.op) { char o[2] = { c.op, 0 }; sys.draw_string(ox + 10, oy + 9, o, mw_ui_color(MW_TH_SHADOW)); }
        sys.draw_string(ox + 10, oy + H - 14, status, mw_ui_color(MW_TH_SHADOW));
        char pressed = 0;
        for (int r = 0; r < 6; r++) for (int k = 0; k < 4; k++) {
            if (mw_ui_button(&ui, ox + 6 + k * 50, oy + 46 + r * 35, 48, 31, keys[r][k])) pressed = keys[r][k][0] == 's' ? 'q' : (keys[r][k][0] == 'x' ? 'Q' : (keys[r][k][0] == 'C' && keys[r][k][1] == 'p' ? 'Y' : (keys[r][k][0] == 'P' ? 'V' : (keys[r][k][0] == '+' && keys[r][k][1] == '/' ? 'n' : keys[r][k][0]))));
        }
        mw_ui_end(&ui);
        char k = pressed ? pressed : (char)ui.key;
        status[0] = 0;
        if (k == MW_KEY_ESC) break;
        if (mw_isdigit(k)) digit(&c, k - '0');
        else if (k == '.') { if (!c.err && !c.dot) { if (c.fresh) { c.mag = 0; c.neg = 0; c.fd = 0; c.fresh = 0; } c.dot = 1; } }
        else if (k == '+' || k == '-' || k == '*' || k == '/') press_op(&c, k);
        else if (k == '=' || k == '\n') press_eq(&c);
        else if (k == 'C' || k == 'c') clear(&c);
        else if (k == 'n') { if (!c.err) set_cur(&c, -c.cur), c.fresh = 1; }
        else if (k == '%') { if (!c.err) { int v = c.op ? mw_muldiv(c.acc, c.cur, 100 * SCALE) : mw_muldiv(c.cur, 1, 100); set_cur(&c, v); c.fresh = 1; } }
        else if (k == 'q') { if (!c.err) { if (c.cur < 0) c.err = 1; else { set_cur(&c, (int)mw_isqrt64((mw_u64)(unsigned)c.cur * SCALE)); c.fresh = 1; } } }
        else if (k == 'Q') { if (!c.err) { set_cur(&c, mw_muldiv(c.cur, c.cur, SCALE)); c.fresh = 1; } }
        else if (k == 'Y') { show(&c, text, sizeof text); mw_clip_copy(text); mw_strlcpy(status, "copied", sizeof status); }
        else if (k == 'V') { paste(&c); }
        else if (k == '<' || k == '\b') { if (!c.err && !c.fresh && (c.mag > 0 || c.dot)) { if (c.dot && c.fd > 0) { static const unsigned p[3] = { 100, 10, 1 }; unsigned d = (c.mag % SCALE) / p[c.fd - 1] % 10; c.mag -= d * p[c.fd - 1]; c.fd--; } else if (c.dot) c.dot = 0; else c.mag = c.mag / 10 / SCALE * SCALE; c.cur = c.neg ? -(int)c.mag : (int)c.mag; } }
    }
}
