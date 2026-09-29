#!/usr/bin/env python3
"""Reference vectors for host_crypto_test.c, produced by Python's hashlib/hmac
and the `cryptography` package (i.e. OpenSSL) -- NOT by MiniWin's own code, so a
mismatch means MiniWin is wrong. Deterministic (seeded) so failures reproduce.

Line formats (hex, "-" = empty):
  SHA384 <msg> <digest>          SHA512 <msg> <digest>
  HMAC384 <key> <msg> <mac>
  GCM <key> <iv> <aad> <plaintext> <ciphertext> <tag>
  (ECC / ECDSA lines are appended by later steps of the TLS overhaul)
"""
import hashlib, hmac, random, sys
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

rnd = random.Random(20260929)
def rb(n): return bytes(rnd.randrange(256) for _ in range(n))
def h(b): return b.hex() if b else "-"

out = []
# hash lengths straddling the 128-byte block and the 112-byte padding threshold
for n in list(range(0, 5)) + [55, 56, 63, 64, 65, 111, 112, 113, 127, 128, 129, 200, 255, 256, 257, 1000, 4096]:
    m = rb(n)
    out.append("SHA384 %s %s" % (h(m), hashlib.sha384(m).hexdigest()))
    out.append("SHA512 %s %s" % (h(m), hashlib.sha512(m).hexdigest()))
for klen in [0, 1, 20, 47, 48, 128, 129, 200]:
    for mlen in [0, 1, 50, 128, 300]:
        k, m = rb(klen), rb(mlen)
        out.append("HMAC384 %s %s %s" % (h(k), h(m), hmac.new(k, m, hashlib.sha384).hexdigest()))
for keylen in (16, 32):
    for ptlen in [0, 1, 15, 16, 17, 31, 32, 33, 100, 1000, 4097, 16384]:
        for aadlen in (0, 13, 20):
            key, iv, aad, pt = rb(keylen), rb(12), rb(aadlen), rb(ptlen)
            sealed = AESGCM(key).encrypt(iv, pt, aad or None)
            ct, tag = sealed[:-16], sealed[-16:]
            out.append("GCM %s %s %s %s %s %s" % (h(key), h(iv), h(aad), h(pt), h(ct), tag.hex()))
open(sys.argv[1] if len(sys.argv) > 1 else "/tmp/crypto_vectors.txt", "w").write("\n".join(out) + "\n")
print("wrote", len(out), "vectors")
