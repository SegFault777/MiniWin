#!/usr/bin/env python3
"""Prints the C constants for kernel/ecc.h (NIST P-256 and P-384, from FIPS 186-4 /
SEC 2) as little-endian u32 limb arrays, after CHECKING them with plain Python
big-int arithmetic: G is on the curve, and n*G is the point at infinity. A typo in a
hand-copied 96-hex-digit constant would otherwise surface as "ECDSA verify never
succeeds" -- this makes it fail here instead, loudly, at generation time."""
P256 = dict(
 p=0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff,
 n=0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551,
 b=0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b,
 gx=0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296,
 gy=0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5)
P384 = dict(
 p=0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff,
 n=0xffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973,
 b=0xb3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef,
 gx=0xaa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7,
 gy=0x3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f)

def inv(a, m): return pow(a, -1, m)
def add(P, Q, p, a):
    if P is None: return Q
    if Q is None: return P
    (x1, y1), (x2, y2) = P, Q
    if x1 == x2 and (y1 + y2) % p == 0: return None
    l = ((3*x1*x1 + a) * inv(2*y1, p) if P == Q else (y2 - y1) * inv(x2 - x1, p)) % p
    x3 = (l*l - x1 - x2) % p
    return (x3, (l*(x1 - x3) - y1) % p)
def mul(k, P, p, a):
    R = None
    while k:
        if k & 1: R = add(R, P, p, a)
        P = add(P, P, p, a); k >>= 1
    return R

def limbs(v, n): return ", ".join("0x%08x" % ((v >> (32*i)) & 0xffffffff) for i in range(n))
for name, c, nl in (("P256", P256, 8), ("P384", P384, 12)):
    p, a = c["p"], c["p"] - 3
    G = (c["gx"], c["gy"])
    assert (G[1]**2 - (G[0]**3 + a*G[0] + c["b"])) % p == 0, name + ": G not on curve"
    assert mul(c["n"], G, p, a) is None, name + ": n*G != infinity"
    print("/* %s -- checked: G on curve, n*G = infinity */" % name)
    for k in ("p", "n", "b", "gx", "gy"):
        print("#define ECC_%s_%s { %s }" % (name, k.upper(), limbs(c[k], nl)))
