/* host_crypto_test.c -- compiles MiniWin's REAL crypto headers on the build
 * host (they are freestanding C, so they build fine against a hosted libc) and
 * checks them against vectors from gen_crypto_vectors.py.
 *
 *   tools/test/run_host_crypto.sh
 *
 * Exit status 0 = every vector matched. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/io.h"
#include "../../kernel/sha512.h"
#include "../../kernel/hmac_sha384.h"
#include "../../kernel/aes.h"
#include "../../kernel/gcm.h"
#ifdef HAVE_ECC_TESTS
#include "ecc_tests.h"
#endif

static int hexval(char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }
static u32 unhex(const char *s, u8 *out) {
    if (s[0] == '-' && s[1] == 0) return 0;
    u32 n = (u32)strlen(s) / 2;
    for (u32 i = 0; i < n; i++) out[i] = (u8)(hexval(s[2*i]) << 4 | hexval(s[2*i+1]));
    return n;
}

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv) {
    FILE *f = fopen(argc > 1 ? argv[1] : "/tmp/crypto_vectors.txt", "r");
    if (!f) { perror("vectors"); return 2; }
    static char line[200000];
    static u8 a[70000], b[70000], c[70000], d[70000], e[70000], g[70000];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tok[8]; int nt = 0;
        for (char *p = strtok(line, " "); p && nt < 8; p = strtok(NULL, " ")) tok[nt++] = p;
        if (nt == 0) continue;
        if (!strcmp(tok[0], "SHA384") || !strcmp(tok[0], "SHA512")) {
            int is384 = tok[0][3] == '3';
            u32 n = unhex(tok[1], a), dl = unhex(tok[2], b);
            u8 out[64];
            /* one-shot AND a byte-at-a-time stream (exercises the buffering) */
            if (is384) sha384(a, n, out); else sha512(a, n, out);
            CHECK(memcmp(out, b, dl) == 0, "%s one-shot len=%u", tok[0], n);
            sha512_ctx_t cx; if (is384) sha384_init(&cx); else sha512_init(&cx);
            for (u32 i = 0; i < n; i++) sha512_update(&cx, a + i, 1);
            if (is384) sha384_final(&cx, out); else sha512_final(&cx, out);
            CHECK(memcmp(out, b, dl) == 0, "%s streamed len=%u", tok[0], n);
        } else if (!strcmp(tok[0], "HMAC384")) {
            u32 kl = unhex(tok[1], a), ml = unhex(tok[2], b); unhex(tok[3], c);
            u8 out[48]; hmac_sha384(a, kl, b, ml, out);
            CHECK(memcmp(out, c, 48) == 0, "HMAC384 klen=%u mlen=%u", kl, ml);
        } else if (!strcmp(tok[0], "GCM")) {
            u32 kl = unhex(tok[1], a); unhex(tok[2], b);
            u32 al = unhex(tok[3], c), pl = unhex(tok[4], d);
            unhex(tok[5], e); u8 tag_ref[16]; unhex(tok[6], tag_ref);
            aes_ctx_t ctx; aes_set_key(&ctx, a, (int)kl);
            memcpy(g, d, pl); u8 tag[16];
            gcm_encrypt(&ctx, b, c, al, g, pl, tag);
            CHECK(memcmp(g, e, pl) == 0, "GCM-%u encrypt ct pt=%u aad=%u", kl * 8, pl, al);
            CHECK(memcmp(tag, tag_ref, 16) == 0, "GCM-%u encrypt tag pt=%u aad=%u", kl * 8, pl, al);
            int ok = gcm_decrypt(&ctx, b, c, al, g, pl, tag_ref);
            CHECK(ok && memcmp(g, d, pl) == 0, "GCM-%u decrypt round trip pt=%u", kl * 8, pl);
            u8 bad[16]; memcpy(bad, tag_ref, 16); bad[5] ^= 1;
            memcpy(g, e, pl);
            CHECK(gcm_decrypt(&ctx, b, c, al, g, pl, bad) == 0, "GCM-%u must reject a bad tag pt=%u", kl * 8, pl);
            CHECK(memcmp(g, e, pl) == 0, "GCM-%u must leave ciphertext untouched on a bad tag", kl * 8);
        }
#ifdef HAVE_ECC_TESTS
        else ecc_test_line(tok, nt, &checks, &failures);
#endif
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
