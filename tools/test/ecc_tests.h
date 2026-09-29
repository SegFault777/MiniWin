/* ecc_tests.h -- the ECC/ECDSA/ECDH half of host_crypto_test.c (pulled in with
 * -DHAVE_ECC_TESTS). Vectors come from `cryptography`/OpenSSL, see gen_crypto_vectors.py. */
#include "../../kernel/ecc.h"

static u32 ecc_unhex(const char *s, u8 *out) {
    u32 n = (u32)strlen(s) / 2;
    for (u32 i = 0; i < n; i++) out[i] = (u8)(hexval(s[2*i]) << 4 | hexval(s[2*i+1]));
    return n;
}

static void ecc_test_line(char **tok, int nt, int *checks, int *failures) {
    static u8 pub[200], dig[80], sig[200], rnd[64], peer[200], ours[200], shared[64], priv[64], got_pub[200], got_sh[64];
    ecc_curve_t *c;
    if (!strcmp(tok[0], "ECDSA") && nt >= 6) {
        c = atoi(tok[1]) == 256 ? &ecc_p256 : &ecc_p384;
        u32 pl = ecc_unhex(tok[2], pub), dl = ecc_unhex(tok[3], dig), sl = ecc_unhex(tok[4], sig);
        int want = atoi(tok[5]);
        int got = ecdsa_verify_der(c, pub, pl, dig, dl, sig, sl);
        (*checks)++;
        if (got != want) { (*failures)++; printf("FAIL: ECDSA P-%s expected %d got %d (hash len %u)\n", tok[1], want, got, dl); }
    } else if (!strcmp(tok[0], "ECDH") && nt >= 6) {
        c = atoi(tok[1]) == 256 ? &ecc_p256 : &ecc_p384;
        ecc_unhex(tok[2], rnd);
        u32 ql = ecc_unhex(tok[3], peer); ecc_unhex(tok[4], ours); ecc_unhex(tok[5], shared);
        int ok = ecdh_keygen(c, rnd, priv, got_pub);
        (*checks)++;
        if (!ok || memcmp(got_pub, ours, 1 + 2 * c->bytes)) { (*failures)++; printf("FAIL: ECDH P-%s public key mismatch\n", tok[1]); return; }
        ok = ecdh_shared(c, priv, peer, ql, got_sh);
        (*checks)++;
        if (!ok || memcmp(got_sh, shared, c->bytes)) { (*failures)++; printf("FAIL: ECDH P-%s shared secret mismatch\n", tok[1]); }
    } else if (!strcmp(tok[0], "ECBAD") && nt >= 3) {
        c = atoi(tok[1]) == 256 ? &ecc_p256 : &ecc_p384;
        u32 ql = ecc_unhex(tok[2], peer);
        ecc_unhex("11", rnd);
        u8 dummy[64] = {1,2,3};
        (*checks)++;
        if (ecdh_shared(c, dummy, peer, ql, got_sh)) { (*failures)++; printf("FAIL: ECDH P-%s accepted an invalid point\n", tok[1]); }
    }
}
