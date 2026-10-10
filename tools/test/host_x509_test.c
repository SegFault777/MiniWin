/* host_x509_test.c -- certificate-extension handling (audit M-02): critical extensions we do not understand must
 * make a certificate unacceptable, and keyUsage must agree with the certificate's role (TLS server leaf: must
 * allow digitalSignature; issuer: must allow keyCertSign). The DER files are produced by run_host_x509.sh with
 * OpenSSL, one per variant, so the parser is exercised on real encodings rather than hand-built bytes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/io.h"
#include "../../kernel/sha512.h"
#include "../../kernel/hmac_sha384.h"
#include "../../kernel/aes.h"
#include "../../kernel/gcm.h"
#include "../../kernel/x509.h"
#include "../../kernel/trusted_roots.h"

static int checks = 0, failures = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned char der[4096];
static x509_cert_t cert;
static int load(const char *dir, const char *name) {
    char path[256]; snprintf(path, sizeof path, "%s/%s.der", dir, name);
    FILE *f = fopen(path, "rb"); if (!f) { printf("FAIL: cannot open %s\n", path); failures++; return 0; }
    size_t n = fread(der, 1, sizeof der, f); fclose(f);
    int ok = x509_parse(der, (u32)n, &cert);
    if (!ok) { printf("FAIL: %s does not parse\n", name); failures++; }
    return ok;
}

int main(int argc, char **argv) {
    const char *d = argc > 1 ? argv[1] : "/tmp/x509t";

    /* ---- end-entity (TLS server) certificates ---- */
    if (load(d, "leaf_plain"))        CHECK(!cert.has_unknown_critical && !cert.has_key_usage && x509_leaf_extensions_ok(&cert), "no extensions at all: acceptable");
    if (load(d, "leaf_ku_ds"))        CHECK(cert.has_key_usage && cert.key_usage == X509_KU_DIGITAL_SIGNATURE && x509_leaf_extensions_ok(&cert), "keyUsage=digitalSignature (bits=0x%x): acceptable", cert.key_usage);
    if (load(d, "leaf_ku_ds_ke"))     CHECK(cert.key_usage == (X509_KU_DIGITAL_SIGNATURE | (1u << 2)) && x509_leaf_extensions_ok(&cert), "digitalSignature+keyEncipherment (bits=0x%x): acceptable", cert.key_usage);
    if (load(d, "leaf_ku_noDS"))      CHECK(cert.has_key_usage && !(cert.key_usage & X509_KU_DIGITAL_SIGNATURE) && !x509_leaf_extensions_ok(&cert), "keyUsage without digitalSignature is refused for ECDHE (bits=0x%x)", cert.key_usage);
    if (load(d, "leaf_ku_certsign"))  CHECK(!x509_leaf_extensions_ok(&cert), "a leaf whose only usage is keyCertSign is refused");
    if (load(d, "leaf_unknown_crit")) CHECK(cert.has_unknown_critical && !x509_leaf_extensions_ok(&cert), "an unknown CRITICAL extension (private OID) is refused");
    if (load(d, "leaf_unknown_noncrit")) CHECK(!cert.has_unknown_critical && x509_leaf_extensions_ok(&cert), "the same unknown extension, NON-critical, is ignored (RFC 5280 4.2)");
    if (load(d, "leaf_crldp_crit"))   CHECK(cert.has_unknown_critical, "a critical CRL distribution point is refused (revocation is not checked)");
    if (load(d, "leaf_policies_crit"))CHECK(!cert.has_unknown_critical && x509_leaf_extensions_ok(&cert), "critical certificatePolicies: understood (no policy is demanded)");
    if (load(d, "leaf_eku_crit"))     CHECK(!cert.has_unknown_critical && x509_leaf_extensions_ok(&cert), "critical extendedKeyUsage: understood");
    if (load(d, "leaf_san_crit"))     CHECK(!cert.has_unknown_critical && cert.san_ext_len > 0 && x509_leaf_extensions_ok(&cert), "critical subjectAltName: understood and still read");
    if (load(d, "leaf_bc_crit_false"))CHECK(!cert.has_unknown_critical && !cert.is_ca && x509_leaf_extensions_ok(&cert), "critical basicConstraints CA:FALSE on a leaf: fine");
    if (load(d, "leaf_ku_noncrit_noDS")) CHECK(!x509_leaf_extensions_ok(&cert), "keyUsage is enforced even when not marked critical");

    /* ---- CA certificates (issuers) ---- */
    if (load(d, "ca_ok"))             CHECK(cert.is_ca && cert.key_usage == (X509_KU_KEY_CERT_SIGN | (1u << 6)) && x509_ca_extensions_ok(&cert), "CA:TRUE + keyCertSign,cRLSign (bits=0x%x): acceptable issuer", cert.key_usage);
    if (load(d, "ca_no_ku"))          CHECK(cert.is_ca && !cert.has_key_usage && x509_ca_extensions_ok(&cert), "CA:TRUE with no keyUsage: acceptable issuer");
    if (load(d, "ca_ku_noKCS"))       CHECK(cert.is_ca && cert.has_key_usage && !x509_ca_extensions_ok(&cert), "CA:TRUE but keyUsage lacks keyCertSign: NOT an acceptable issuer (bits=0x%x)", cert.key_usage);
    if (load(d, "notca"))             CHECK(!cert.is_ca && !x509_ca_extensions_ok(&cert), "CA:FALSE is not an issuer");
    if (load(d, "ca_unknown_crit"))   CHECK(cert.is_ca && cert.has_unknown_critical && !x509_ca_extensions_ok(&cert), "an issuer with an unknown critical extension is refused");
    if (load(d, "ca_nameconstraints_crit")) CHECK(cert.is_ca && cert.has_unknown_critical && !x509_ca_extensions_ok(&cert), "critical nameConstraints (not enforced here) => refused rather than silently ignored");
    if (load(d, "ca_nameconstraints_noncrit")) CHECK(cert.is_ca && !cert.has_unknown_critical && x509_ca_extensions_ok(&cert), "NON-critical nameConstraints may be ignored");

    /* ---- every embedded trust anchor must still parse and be usable ---- */
    { int n = 0, bad = 0;
      static x509_cert_t root;
      for (int r = 0; r < TRUSTED_ROOT_COUNT; r++) {
          n++;
          if (!x509_parse(trusted_roots[r], trusted_root_lens[r], &root) || !root.is_ca) bad++;
      }
      CHECK(bad == 0, "all %d embedded roots still parse as CAs (%d did not)", n, bad);
      /* a server may send its root inside the chain; it is then checked like any issuer, so none of the
       * embedded roots may trip the new rules (unknown critical extension / keyUsage without keyCertSign) */
      int refused = 0;
      for (int r = 0; r < TRUSTED_ROOT_COUNT; r++) {
          x509_parse(trusted_roots[r], trusted_root_lens[r], &root);
          if (!x509_ca_extensions_ok(&root)) { refused++; printf("  root #%d refused as issuer: unknown_critical=%d ku=%d/0x%x\n", r, root.has_unknown_critical, root.has_key_usage, root.key_usage); }
      }
      CHECK(refused == 0, "%d of %d embedded roots would be refused as issuers by the new extension rules", refused, n); }
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
