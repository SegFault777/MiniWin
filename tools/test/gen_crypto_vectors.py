#!/usr/bin/env python3
"""Reference vectors for host_crypto_test.c, produced by Python's hashlib/hmac
and the `cryptography` package (i.e. OpenSSL) -- NOT by MiniWin's own code, so a
mismatch means MiniWin is wrong. Deterministic (seeded) so failures reproduce.

Line formats (hex, "-" = empty):
  SHA384 <msg> <digest>          SHA512 <msg> <digest>
  HMAC384 <key> <msg> <mac>
  GCM <key> <iv> <aad> <plaintext> <ciphertext> <tag>
  ECDSA <bits> <pub> <digest> <sigDER> <1|0>     expected verify result
  ECDH  <bits> <rand> <peer_pub> <our_pub> <shared>   (our priv = rand | topbit, reduced mod n)
  ECBAD <bits> <peer_pub>                         a point ecdh_shared() must REJECT
  X509 <leaf.der> <issuer.der> <1|0> <ec_group|0>  x509_verify_signed_by(leaf, issuer) expected result
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
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import hashes, serialization
CURVES = {256: (ec.SECP256R1(), 32, 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551),
          384: (ec.SECP384R1(), 48, 0xffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973)}
def pub_bytes(k): return k.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
for bits, (curve, nb, order) in CURVES.items():
    for i in range(6):
        key = ec.generate_private_key(curve)
        pub = pub_bytes(key)
        for halg, hname in ((hashes.SHA256(), "sha256"), (hashes.SHA384(), "sha384"), (hashes.SHA512(), "sha512"), (hashes.SHA1(), "sha1")):
            msg = rb(rnd.randrange(1, 300))
            dig = hashlib.new(hname, msg).digest()
            sig = key.sign(msg, ec.ECDSA(halg))
            out.append("ECDSA %d %s %s %s 1" % (bits, pub.hex(), dig.hex(), sig.hex()))
            # only flip inside the part of the digest ECDSA actually uses: a hash longer than
            # the curve's order is truncated to its leftmost bytes (SEC1 4.1.4), so a flip out
            # in the discarded tail correctly does NOT change the verdict
            bad = bytearray(dig); bad[rnd.randrange(min(len(bad), nb))] ^= 1 << rnd.randrange(8)
            out.append("ECDSA %d %s %s %s 0" % (bits, pub.hex(), bytes(bad).hex(), sig.hex()))
            sb = bytearray(sig); sb[-1 - rnd.randrange(8)] ^= 1 << rnd.randrange(8)
            out.append("ECDSA %d %s %s %s 0" % (bits, pub.hex(), dig.hex(), bytes(sb).hex()))
        other = pub_bytes(ec.generate_private_key(curve))   # a valid key that did NOT sign this
        out.append("ECDSA %d %s %s %s 0" % (bits, other.hex(), dig.hex(), sig.hex()))
    for i in range(8):
        r = rb(nb)
        d = int.from_bytes(r, "big") | (1 << (nb * 8 - 1))
        if d >= order: d -= order
        mine = ec.derive_private_key(d, curve)
        peer = ec.generate_private_key(curve)
        shared = mine.exchange(ec.ECDH(), peer.public_key())
        out.append("ECDH %d %s %s %s %s" % (bits, r.hex(), pub_bytes(peer).hex(), pub_bytes(mine).hex(), shared.hex()))
    good = pub_bytes(ec.generate_private_key(curve))
    y_bad = bytearray(good); y_bad[-1] ^= 1                       # off the curve
    out.append("ECBAD %d %s" % (bits, bytes(y_bad).hex()))
    out.append("ECBAD %d %s" % (bits, (b"\x04" + b"\x00" * (2 * nb)).hex()))     # (0,0)
    out.append("ECBAD %d %s" % (bits, (b"\x02" + good[1:1 + nb]).hex()))          # compressed: unsupported
    out.append("ECBAD %d %s" % (bits, good[:-1].hex()))            # truncated

# ---- X.509: real certificates, every signature/key combination a public chain uses ----
import datetime
from cryptography import x509 as cx
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives.asymmetric import rsa
def mk_key(kind):
    if kind == "rsa": return rsa.generate_private_key(65537, 2048)
    return ec.generate_private_key(ec.SECP256R1() if kind == "p256" else ec.SECP384R1())
def mk_cert(subject_cn, subject_key, issuer_cn, issuer_key, halg, is_ca):
    now = datetime.datetime(2026, 1, 1)
    b = (cx.CertificateBuilder().subject_name(cx.Name([cx.NameAttribute(NameOID.COMMON_NAME, subject_cn)]))
         .issuer_name(cx.Name([cx.NameAttribute(NameOID.COMMON_NAME, issuer_cn)]))
         .public_key(subject_key.public_key()).serial_number(cx.random_serial_number())
         .not_valid_before(now).not_valid_after(now + datetime.timedelta(days=3650))
         .add_extension(cx.BasicConstraints(ca=is_ca, path_length=None), critical=True))
    return b.sign(issuer_key, halg).public_bytes(serialization.Encoding.DER)
HALGS = {"sha256": hashes.SHA256(), "sha384": hashes.SHA384(), "sha512": hashes.SHA512()}
def eg(kind): return {"rsa": 0, "p256": 23, "p384": 24}[kind]
keys = {k: mk_key(k) for k in ("rsa", "p256", "p384")}
ca_der = {k: mk_cert("CA-" + k, keys[k], "CA-" + k, keys[k], hashes.SHA256() if k != "p384" else hashes.SHA384(), True) for k in keys}
for ca_kind in keys:
    for leaf_kind in keys:
        for hn, ha in HALGS.items():
            leaf_key = mk_key(leaf_kind)
            leaf = mk_cert("leaf-%s-%s" % (leaf_kind, hn), leaf_key, "CA-" + ca_kind, keys[ca_kind], ha, False)
            out.append("X509 %s %s 1 %d" % (leaf.hex(), ca_der[ca_kind].hex(), eg(leaf_kind)))
            wrong = [k for k in keys if k != ca_kind][0]
            out.append("X509 %s %s 0 %d" % (leaf.hex(), ca_der[wrong].hex(), eg(leaf_kind)))   # right subject name irrelevant: signature must fail
            t = bytearray(leaf); t[len(t) // 2 - 40] ^= 0x01                                      # corrupt inside the TBS
            out.append("X509 %s %s 0 %d" % (bytes(t).hex(), ca_der[ca_kind].hex(), eg(leaf_kind)))
open(sys.argv[1] if len(sys.argv) > 1 else "/tmp/crypto_vectors.txt", "w").write("\n".join(out) + "\n")
print("wrote", len(out), "vectors")
