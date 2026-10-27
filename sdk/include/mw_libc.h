/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_LIBC_H
#define MW_LIBC_H

/* mw_libc.h -- the small C library a MiniWin program does not otherwise have (SDK 2.0). Everything is `static inline` or a macro, so
 * only what a program uses ends up in its file (the linker drops the rest), and nothing needs a startup routine or the kernel.
 * No 64-bit division and no floating point (the program is built without libgcc): mw_muldiv() and the fixed-point helpers are the
 * way to do arithmetic that does not fit in 32 bits. */

typedef int            mw_i32;
typedef unsigned long long mw_u64;
#define MW_NULL ((void *)0)

/* ---- memory and strings ---- */
static inline void *mw_memset(void *d, int c, u32 n) { u8 *p = (u8 *)d; while (n--) *p++ = (u8)c; return d; }
static inline void *mw_memcpy(void *d, const void *s, u32 n) { u8 *p = (u8 *)d; const u8 *q = (const u8 *)s; while (n--) *p++ = *q++; return d; }
static inline void *mw_memmove(void *d, const void *s, u32 n) {
    u8 *p = (u8 *)d; const u8 *q = (const u8 *)s;
    if (p < q) { while (n--) *p++ = *q++; } else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
static inline int mw_memcmp(const void *a, const void *b, u32 n) { const u8 *p = (const u8 *)a, *q = (const u8 *)b; while (n--) { if (*p != *q) return (int)*p - (int)*q; p++; q++; } return 0; }
static inline int mw_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static inline int mw_strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (int)(u8)*a - (int)(u8)*b; }
static inline int mw_strncmp(const char *a, const char *b, u32 n) { while (n && *a && *a == *b) { a++; b++; n--; } return n ? (int)(u8)*a - (int)(u8)*b : 0; }
static inline char *mw_strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) { } return r; }
/* copies at most cap-1 characters and ALWAYS terminates (unlike C's strncpy) */
static inline char *mw_strlcpy(char *d, const char *s, u32 cap) { u32 i = 0; if (!cap) return d; while (i + 1 < cap && s[i]) { d[i] = s[i]; i++; } d[i] = 0; return d; }
static inline char *mw_strcat(char *d, const char *s) { char *r = d; while (*d) d++; while ((*d++ = *s++)) { } return r; }
static inline char *mw_strchr(const char *s, int c) { for (; *s; s++) if (*s == (char)c) return (char *)s; return (c == 0) ? (char *)s : 0; }
static inline char *mw_strstr(const char *h, const char *n) {
    if (!*n) return (char *)h;
    for (; *h; h++) { const char *a = h, *b = n; while (*a && *b && *a == *b) { a++; b++; } if (!*b) return (char *)h; }
    return 0;
}
static inline int mw_isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int mw_isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline int mw_isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static inline int mw_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static inline int mw_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
/* "  -42abc" -> -42 (stops at the first non-digit; no overflow check) */
static inline int mw_atoi(const char *s) { int neg = 0, v = 0; while (mw_isspace(*s)) s++; if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++; while (mw_isdigit(*s)) v = v * 10 + (*s++ - '0'); return neg ? -v : v; }

/* ---- numbers ---- */
static inline int mw_abs(int v) { return v < 0 ? -v : v; }
#define mw_min(a, b) ((a) < (b) ? (a) : (b))
#define mw_max(a, b) ((a) > (b) ? (a) : (b))
static inline int mw_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
/* floor(sqrt(n)) for a 64-bit value, by the digit-by-digit method (shifts and subtracts only) */
static inline u32 mw_isqrt64(mw_u64 n) {
    mw_u64 res = 0, bit = 1ull << 62;
    while (bit > n) bit >>= 2;
    while (bit) { if (n >= res + bit) { n -= res + bit; res = (res >> 1) + bit; } else res >>= 1; bit >>= 2; }
    return (u32)res;
}
static inline u32 mw_isqrt(u32 n) { return mw_isqrt64(n); }
/* (a * b) / c with a 64-bit intermediate (one imul, one idiv), rounded toward zero; saturates to INT_MIN/MAX when the result does
 * not fit in 32 bits (and c == 0), instead of faulting like a bare idiv would. */
static inline int mw_muldiv(int a, int b, int c) {
    long long p = (long long)a * (long long)b;
    int neg = (p < 0) != (c < 0);
    mw_u64 ap = (mw_u64)(p < 0 ? -p : p);
    u32 ac = (u32)(c < 0 ? -c : c);
    if (ac == 0 || (u32)(ap >> 32) >= ac) return neg ? (int)0x80000000 : 0x7FFFFFFF;
    u32 q, r; u32 hi = (u32)(ap >> 32), lo = (u32)ap;
    __asm__("divl %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "r"(ac) : "cc");
    if (q > 0x7FFFFFFFu) return neg ? (int)0x80000000 : 0x7FFFFFFF;
    return neg ? -(int)q : (int)q;
}
/* sine and cosine of an angle in DEGREES, as fixed point: the result is -256..256 (256 = 1.0) */
static inline int mw_sin(int deg) {
    static const unsigned short t[91] = { 0,4,9,13,18,22,27,31,36,40,44,49,53,58,62,66,71,75,79,83,88,92,96,100,104,108,112,116,120,124,128,132,136,139,143,147,150,154,158,161,165,168,171,175,178,181,184,187,190,193,196,199,202,204,207,210,212,215,217,219,222,224,226,228,230,232,234,236,237,239,241,242,243,245,246,247,248,249,250,251,252,253,254,254,255,255,255,256,256,256,256 };
    deg %= 360; if (deg < 0) deg += 360;
    int sign = 1; if (deg >= 180) { sign = -1; deg -= 180; }
    if (deg > 90) deg = 180 - deg;
    return sign * (int)t[deg];
}
static inline int mw_cos(int deg) { return mw_sin(deg + 90); }

/* ---- text formatting ----
 * mw_snprintf(buf, cap, fmt, ...): %d %i %u %x %X %c %s %% with an optional '-' (left align), '0' (zero pad) and width, e.g. "%02d:%02d",
 * "%-8s", "%5u". Always terminates; returns the length it WOULD have written (like C99), so a caller can detect truncation. */
static inline int mw_vsnprintf(char *buf, u32 cap, const char *fmt, __builtin_va_list ap) {
    u32 n = 0;
    #define MW_PUT(ch) do { if (n + 1 < cap) buf[n] = (char)(ch); n++; } while (0)
    for (; *fmt; fmt++) {
        if (*fmt != '%') { MW_PUT(*fmt); continue; }
        fmt++;
        int left = 0, zero = 0, width = 0;
        for (;; fmt++) { if (*fmt == '-') left = 1; else if (*fmt == '0') zero = 1; else break; }
        while (mw_isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        char tmp[16]; int tl = 0; const char *s = 0; int neg = 0;
        char c = *fmt;
        if (c == 'd' || c == 'i') { int v = __builtin_va_arg(ap, int); u32 u = v < 0 ? (u32)(-v) : (u32)v; neg = v < 0; do { tmp[tl++] = (char)('0' + u % 10u); u /= 10u; } while (u); }
        else if (c == 'u') { u32 u = __builtin_va_arg(ap, u32); do { tmp[tl++] = (char)('0' + u % 10u); u /= 10u; } while (u); }
        else if (c == 'x' || c == 'X') { u32 u = __builtin_va_arg(ap, u32); do { u32 d = u & 15u; tmp[tl++] = (char)(d < 10 ? '0' + d : (c == 'x' ? 'a' : 'A') + d - 10); u >>= 4; } while (u); }
        else if (c == 'c') { tmp[tl++] = (char)__builtin_va_arg(ap, int); }
        else if (c == 's') { s = __builtin_va_arg(ap, const char *); if (!s) s = "(null)"; }
        else if (c == '%') { MW_PUT('%'); continue; }
        else if (!c) break;
        else { MW_PUT('%'); MW_PUT(c); continue; }
        int len = s ? mw_strlen(s) : tl + neg;
        int pad = width > len ? width - len : 0;
        if (!left && !(zero && !s)) while (pad--) MW_PUT(' ');
        if (!s && neg) MW_PUT('-');
        if (!left && zero && !s) while (pad--) MW_PUT('0');
        if (s) { for (int i = 0; s[i]; i++) MW_PUT(s[i]); } else while (tl) MW_PUT(tmp[--tl]);
        if (left) while (pad--) MW_PUT(' ');
    }
    if (cap) buf[n < cap ? n : cap - 1] = 0;
    #undef MW_PUT
    return (int)n;
}
static inline int mw_snprintf(char *buf, u32 cap, const char *fmt, ...) {
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int r = mw_vsnprintf(buf, cap, fmt, ap);
    __builtin_va_end(ap);
    return r;
}

/* ---- sorting: a plain insertion sort (fine for the few dozen items a 24 KB program handles) ---- */
static inline void mw_sort(void *base, u32 count, u32 size, int (*cmp)(const void *, const void *)) {
    u8 *a = (u8 *)base, tmp[64];
    if (size > sizeof tmp) return;
    for (u32 i = 1; i < count; i++) {
        mw_memcpy(tmp, a + i * size, size);
        u32 j = i;
        while (j > 0 && cmp(a + (j - 1) * size, tmp) > 0) { mw_memcpy(a + j * size, a + (j - 1) * size, size); j--; }
        mw_memcpy(a + j * size, tmp, size);
    }
}

#endif
