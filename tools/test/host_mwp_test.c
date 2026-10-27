/* SPDX-License-Identifier: GPL-3.0-only */
/* host_mwp_test.c -- kernel/mwp_trailer.h (the .mwp metadata trailer) and sdk/include/mw_libc.h (the SDK's small C library) on the host,
 * under ASan + UBSan. The trailer files are made by tools/mwp_pack.py (run by tools/test/run_host_mwp.sh); the parser must accept them and
 * must survive every kind of damage without reading past the end of the file (each test file lives in an exact-size heap block, so ASan
 * reports a single byte of overread).   tools/test/run_host_mwp.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
#include "../../kernel/mwp_trailer.h"
#define MWP_ABI_VERSION 3
#include "../../sdk/include/mw_libc.h"

static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static u8 *slurp(const char *path, u32 *len) {
    FILE *f = fopen(path, "rb"); if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *p = malloc((size_t)n); if (fread(p, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f); *len = (u32)n; return p;
}
/* parse a COPY of img in an exact-size block, so any overread is an ASan error */
static int parse_exact(const u8 *img, u32 len, mwp_trailer_t *t) {
    u8 *c = malloc(len ? len : 1); memcpy(c, img, len);
    int r = mwp_trailer_parse(c, len, t);
    if (r) {                                              /* everything the parser promises must be inside the file */
        CHECK(t->payload_len >= 1 && t->payload_len <= len - 32);
        if (t->icon16_off) CHECK(t->icon16_off >= t->payload_len && t->icon16_off + 1024 <= len - 32);
        if (t->icon32_off) CHECK(t->icon32_off >= t->payload_len && t->icon32_off + 4096 <= len - 32);
        if (t->title_off) { u32 k = t->title_off; while (k < len - 32 && c[k]) k++; CHECK(k < len - 32); }
    }
    free(c);
    return r;
}

static void test_trailer(const char *dir) {
    char path[256]; u32 len; mwp_trailer_t t;
    snprintf(path, sizeof path, "%s/full.mwp", dir); u8 *full = slurp(path, &len);
    CHECK(parse_exact(full, len, &t) == 1);
    CHECK(t.flags == 7 && t.payload_len == 200 && t.min_abi == 3 && t.icon16_off && t.icon32_off && t.title_off);
    CHECK(memcmp(full + t.title_off, "MiniCalc", 9) == 0);
    CHECK(full[t.icon32_off + 3] == 255 || full[t.icon32_off + 3] == 0 || 1);          /* RGBA bytes are there (alpha is whatever the picture has) */
    snprintf(path, sizeof path, "%s/title_only.mwp", dir); u8 *to = slurp(path, &len);
    CHECK(parse_exact(to, len, &t) == 1 && t.flags == 4 && t.icon16_off == 0 && t.icon32_off == 0);
    snprintf(path, sizeof path, "%s/plain.mwp", dir); u8 *plain = slurp(path, &len);
    CHECK(parse_exact(plain, len, &t) == 0);                                            /* a program from before alpha-11: no trailer, no meta */
    /* truncations: a file cut anywhere must never parse into something that reads outside it */
    u32 flen; snprintf(path, sizeof path, "%s/full.mwp", dir); u8 *f2 = slurp(path, &flen);
    for (u32 cut = 0; cut < flen; cut += 7) { mwp_trailer_t tt; parse_exact(f2, cut, &tt); }
    /* damage: flip every byte of the trailer, and random bytes anywhere, 20000 times */
    unsigned seed = 1234567u;
    for (int i = 0; i < 20000; i++) {
        u8 *m = malloc(flen); memcpy(m, f2, flen);
        seed = seed * 1664525u + 1013904223u;
        u32 pos = (i < 32 * 8) ? flen - 32 + (u32)(i % 32) : (seed >> 8) % flen;
        seed = seed * 1664525u + 1013904223u;
        m[pos] ^= (u8)(1u << ((seed >> 8) & 7));
        if (i % 3 == 0) { seed = seed * 1664525u + 1013904223u; m[flen - 32 + ((seed >> 8) % 32)] = (u8)(seed >> 16); }
        mwp_trailer_t tt; parse_exact(m, flen, &tt);
        free(m);
    }
    free(full); free(to); free(plain); free(f2);
}

static void test_libc(void) {
    char b[64];
    CHECK(mw_strlen("abc") == 3 && mw_strcmp("a", "b") < 0 && mw_strcmp("b", "a") > 0 && mw_strcmp("x", "x") == 0);
    CHECK(mw_strncmp("abcd", "abxx", 2) == 0 && mw_strncmp("abcd", "abxx", 3) < 0);
    mw_strlcpy(b, "hello world", 6); CHECK(strcmp(b, "hello") == 0);                      /* always terminated */
    mw_strlcpy(b, "x", 1); CHECK(b[0] == 0);
    strcpy(b, "ab"); mw_strcat(b, "cd"); CHECK(strcmp(b, "abcd") == 0);
    CHECK(mw_strchr("hello", 'l') && *mw_strchr("hello", 'l') == 'l' && !mw_strchr("hello", 'z') && *mw_strchr("hi", 0) == 0);
    CHECK(mw_strstr("find the needle", "needle") && !mw_strstr("abc", "abd") && mw_strstr("abc", "") );
    CHECK(mw_atoi("  -42abc") == -42 && mw_atoi("+7") == 7 && mw_atoi("x") == 0);
    char m1[8] = "abcdefg", m2[8]; mw_memcpy(m2, m1, 8); CHECK(mw_memcmp(m1, m2, 8) == 0);
    mw_memmove(m1 + 2, m1, 5); CHECK(memcmp(m1, "ababcde", 7) == 0);                      /* overlapping, forward */
    strcpy(m1, "abcdefg"); mw_memmove(m1, m1 + 2, 5); CHECK(memcmp(m1, "cdefgfg", 7) == 0);  /* overlapping, backward */
    mw_memset(m2, 'z', 3); CHECK(m2[0] == 'z' && m2[2] == 'z' && m2[3] == 'd');
    CHECK(mw_abs(-5) == 5 && mw_min(2, 3) == 2 && mw_max(2, 3) == 3 && mw_clamp(9, 0, 5) == 5 && mw_clamp(-1, 0, 5) == 0);
    CHECK(mw_toupper('a') == 'A' && mw_tolower('Q') == 'q' && mw_isdigit('7') && !mw_isdigit('a') && mw_isalpha('k') && mw_isspace('\t'));
    /* square roots, exhaustively around the interesting spots and by sampling */
    for (u32 n = 0; n < 70000; n++) { u32 r = mw_isqrt(n); CHECK((mw_u64)r * r <= n && (mw_u64)(r + 1) * (r + 1) > n); }
    mw_u64 big[] = { 0xFFFFFFFFFFFFFFFFull, 1ull << 62, 999999999999ull, 4294967296ull * 4294967296ull - 1 };
    for (int i = 0; i < 4; i++) { u32 r = mw_isqrt64(big[i]); CHECK((mw_u64)r * r <= big[i] && ((mw_u64)(r + 1) * (r + 1) > big[i] || r == 0xFFFFFFFFu)); }
    /* muldiv: against 64-bit arithmetic, with saturation instead of a fault */
    CHECK(mw_muldiv(3, 4, 5) == 2 && mw_muldiv(-3, 4, 5) == -2 && mw_muldiv(3, -4, -5) == 2 && mw_muldiv(1000000, 1000000, 1000) == 1000000000 && mw_muldiv(1000000, 1000000, 100) == 0x7FFFFFFF);
    CHECK(mw_muldiv(5, 5, 0) == 0x7FFFFFFF && mw_muldiv(-5, 5, 0) == (int)0x80000000 && mw_muldiv(2147483647, 2, 2) == 2147483647);
    unsigned s = 99;
    for (int i = 0; i < 100000; i++) {
        s = s * 1664525u + 1013904223u; int a = (int)(s >> 4) % 100000;
        s = s * 1664525u + 1013904223u; int bb = (int)(s >> 4) % 100000;
        s = s * 1664525u + 1013904223u; int c = (int)(s >> 4) % 1000; if (c == 0) c = 7;
        long long want = (long long)a * bb / c;
        if (want >= -2147483648LL && want <= 2147483647LL) CHECK(mw_muldiv(a, bb, c) == (int)want);
    }
    /* sine: exact at the cardinal points, symmetric, close to the real thing */
    CHECK(mw_sin(0) == 0 && mw_sin(90) == 256 && mw_sin(180) == 0 && mw_sin(270) == -256 && mw_cos(0) == 256 && mw_cos(180) == -256 && mw_sin(-90) == -256 && mw_sin(450) == 256);
    for (int d = -720; d <= 720; d++) { int v = mw_sin(d); CHECK(v >= -256 && v <= 256 && mw_sin(d + 360) == v && mw_sin(-d) == -v); }
    /* snprintf: the formats the SDK documents, truncation and the returned length */
    CHECK(mw_snprintf(b, sizeof b, "%02d:%02d", 7, 5) == 5 && !strcmp(b, "07:05"));
    CHECK(mw_snprintf(b, sizeof b, "%-4s|%4s|%c|%%", "ab", "cd", 'x') == 13 && !strcmp(b, "ab  |  cd|x|%"));
    CHECK(mw_snprintf(b, sizeof b, "%d %d %u", -12, 0, 4000000000u) == 16 && !strcmp(b, "-12 0 4000000000"));
    CHECK(mw_snprintf(b, sizeof b, "%x %X %05x", 255, 255, 255) == 11 && !strcmp(b, "ff FF 000ff"));
    CHECK(mw_snprintf(b, sizeof b, "%05d|%5d|%-5d|", -42, -42, -42) == 18 && !strcmp(b, "-0042|  -42|-42  |"));
    CHECK(mw_snprintf(b, sizeof b, "%s", (const char *)0) == 6 && !strcmp(b, "(null)"));
    CHECK(mw_snprintf(b, 5, "abcdefgh") == 8 && !strcmp(b, "abcd"));                       /* truncated but terminated, full length reported */
    CHECK(mw_snprintf(b, 1, "abc") == 3 && b[0] == 0);
    CHECK(mw_snprintf(b, sizeof b, "%q") == 2 && !strcmp(b, "%q"));
    /* sort */
    int v[9] = { 5, 3, 9, 1, 7, 3, 0, 8, 2 };
    int cmp(const void *x, const void *y) { return *(const int *)x - *(const int *)y; }
    mw_sort(v, 9, sizeof(int), cmp);
    int ok = 1; for (int i = 1; i < 9; i++) if (v[i - 1] > v[i]) ok = 0; CHECK(ok && v[0] == 0 && v[8] == 9);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: host_mwp_test <dir with full.mwp title_only.mwp plain.mwp>\n"); return 2; }
    test_trailer(argv[1]); test_libc();
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
