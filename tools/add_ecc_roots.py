#!/usr/bin/env python3
"""Appends the Mozilla ECDSA roots (NIST P-256 / P-384 keys) to kernel/trusted_roots.h.

The original 41 anchors were RSA-only (the client could only verify RSA). With ecc.h and
the ECDSA-aware x509.h, the chains that root in an ECC anchor -- Let's Encrypt's ISRG Root
X2, Google Trust Services R3/R4, Amazon Root CA 3/4, DigiCert G3, USERTrust/COMODO ECC,
GlobalSign ECC, Microsoft ECC, ... -- can finally be verified, so their roots are added.

Source: Python's certifi bundle (= Mozilla's root store). Only anchors whose key is on a curve
ecc.h implements are taken, deduplicated against what the header already holds. Idempotent:
re-running adds nothing new. Usage: tools/add_ecc_roots.py  (from the repo root)"""
import re, sys, certifi
from cryptography import x509
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec

PATH = "kernel/trusted_roots.h"
src = open(PATH).read()

# DER of everything already present
have = set()
for m in re.finditer(r"static const u8 ROOT_(\d+)\[\] = \{(.*?)\};", src, re.S):
    have.add(bytes(int(x) for x in re.findall(r"\d+", m.group(2))))
count = int(re.search(r"#define TRUSTED_ROOT_COUNT (\d+)", src).group(1))
assert count == len(have), (count, len(have))

pem = open(certifi.where()).read()
new = []
for block in re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", pem, re.S):
    cert = x509.load_pem_x509_certificate(block.encode())
    key = cert.public_key()
    if not isinstance(key, ec.EllipticCurvePublicKey) or key.curve.name not in ("secp256r1", "secp384r1"):
        continue
    der = cert.public_bytes(serialization.Encoding.DER)
    if der in have:
        continue
    cn = cert.subject.get_attributes_for_oid(x509.NameOID.COMMON_NAME)
    name = cn[0].value if cn else cert.subject.rfc4514_string()
    new.append((name, der))
new.sort(key=lambda t: t[0])
if not new:
    print("nothing to add"); sys.exit(0)

arrays = []
for i, (name, der) in enumerate(new):
    idx = count + i
    body = ",\n".join("    " + ",".join(str(b) for b in der[j:j + 16]) for j in range(0, len(der), 16))
    arrays.append("/* %s (ECDSA) */\nstatic const u32 ROOT_%02d_len = %d;\nstatic const u8 ROOT_%02d[] = {\n%s\n};\n" % (name.replace("*/", ""), idx, len(der), idx, body))
    print("adding ROOT_%02d %s (%d bytes)" % (idx, name, len(der)))

total = count + len(new)
# insert the new arrays right before the count define, and extend both index tables
src = src.replace("#define TRUSTED_ROOT_COUNT %d" % count, "".join(a + "\n" for a in arrays) + "#define TRUSTED_ROOT_COUNT %d" % total, 1)
def extend(table_re, suffix):
    global src
    m = re.search(table_re, src, re.S)
    add = "".join("    ROOT_%02d%s,\n" % (count + i, suffix) for i in range(len(new)))
    src = src[:m.end() - 2] + add + src[m.end() - 2:]
extend(r"static const u8 \*trusted_roots\[TRUSTED_ROOT_COUNT\] = \{.*?\n\};", "")
extend(r"static const u32 trusted_root_lens\[TRUSTED_ROOT_COUNT\] = \{.*?\n\};", "_len")
open(PATH, "w").write(src)
print("trusted roots: %d -> %d" % (count, total))
