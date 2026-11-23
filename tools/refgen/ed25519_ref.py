# SPDX-License-Identifier: Apache-2.0
"""Independent reference Ed25519 for YapChain test vectors.

Pure Python, no libsodium. Arithmetic follows RFC 8032 section 5.1 / 6;
verification follows the normative algorithm in docs/protocol.md section 5.2
(ADR-0014). Slow and NOT constant time: test-vector generation only.
"""
import hashlib

P = 2**255 - 19
L = 2**252 + 27742317777372353535851937790883648493
D = (-121665 * pow(121666, P - 2, P)) % P
SQRT_M1 = pow(2, (P - 1) // 4, P)

SMALL_Y = frozenset({
    0,
    1,
    P - 1,
    2707385501144840649318225287225658788936804267575313519463743609750303402022,
    55188659117513257062467267217118295137698188065244968500265048394206261417927,
})

IDENTITY = (0, 1, 1, 0)


def _inv(x):
    return pow(x, P - 2, P)


def point_add(a, b):
    x1, y1, z1, t1 = a
    x2, y2, z2, t2 = b
    pa = (y1 - x1) * (y2 - x2) % P
    pb = (y1 + x1) * (y2 + x2) % P
    pc = t1 * 2 * D * t2 % P
    pd = z1 * 2 * z2 % P
    e, f, g, h = pb - pa, pd - pc, pd + pc, pb + pa
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def point_neg(a):
    x, y, z, t = a
    return ((-x) % P, y, z, (-t) % P)


def scalar_mul(s, pt):
    q = IDENTITY
    while s > 0:
        if s & 1:
            q = point_add(q, pt)
        pt = point_add(pt, pt)
        s >>= 1
    return q


def point_equal(a, b):
    x1, y1, z1, _ = a
    x2, y2, z2, _ = b
    return (x1 * z2 - x2 * z1) % P == 0 and (y1 * z2 - y2 * z1) % P == 0


def recover_x(y, sign):
    """RFC 8032 5.1.3 steps 2-4. Returns None if no square root exists."""
    if y >= P:
        return None
    x2 = (y * y - 1) * _inv(D * y * y + 1) % P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P != 0:
        x = x * SQRT_M1 % P
    if (x * x - x2) % P != 0:
        return None
    if (x & 1) != sign:
        x = P - x
    return x


_BY = 4 * _inv(5) % P
_BX = recover_x(_BY, 0)
BASE = (_BX, _BY, 1, _BX * _BY % P)


def compress(pt):
    x, y, z, _ = pt
    zi = _inv(z)
    x, y = x * zi % P, y * zi % P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def decompress_canonical(s):
    """RFC 8032 decoding; rejects y >= p."""
    if len(s) != 32:
        return None
    v = int.from_bytes(s, "little")
    sign = v >> 255
    y = v & ((1 << 255) - 1)
    x = recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % P)


def decompress_any(s):
    """Decoding that tolerates non-canonical y (y >= p), used to build edge cases."""
    v = int.from_bytes(s, "little")
    sign = v >> 255
    y = (v & ((1 << 255) - 1)) % P
    x = recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % P)


def y_raw(s):
    return int.from_bytes(s, "little") & ((1 << 255) - 1)


def sha512_int(*parts):
    h = hashlib.sha512()
    for p in parts:
        h.update(p)
    return int.from_bytes(h.digest(), "little")


def expand_seed(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def public_key(seed):
    a, _ = expand_seed(seed)
    return compress(scalar_mul(a, BASE))


def sign(seed, msg):
    a, prefix = expand_seed(seed)
    pub = compress(scalar_mul(a, BASE))
    r = sha512_int(prefix, msg) % L
    rs = compress(scalar_mul(r, BASE))
    k = sha512_int(rs, pub, msg) % L
    s = (r + k * a) % L
    return rs + int.to_bytes(s, 32, "little")


def verify(pub, msg, sig):
    """docs/protocol.md section 5.2, steps 1-7."""
    if len(pub) != 32 or len(sig) != 64:
        return False
    rs, ss = sig[:32], sig[32:]
    s = int.from_bytes(ss, "little")
    if s >= L:                                  # step 1
        return False
    if y_raw(rs) % P in SMALL_Y:                # step 2
        return False
    ya = y_raw(pub)
    if ya >= P:                                 # step 3
        return False
    if ya in SMALL_Y:                           # step 4
        return False
    a_pt = decompress_canonical(pub)            # step 5
    if a_pt is None:
        return False
    k = sha512_int(rs, pub, msg) % L            # step 6
    r_check = point_add(scalar_mul(s, BASE), point_neg(scalar_mul(k, a_pt)))
    return compress(r_check) == rs              # step 7


def torsion_point_of_order_8():
    """Deterministically find a point of exact order 8."""
    for y in range(2, 1000):
        x = recover_x(y, 0)
        if x is None:
            continue
        pt = (x, y, 1, x * y % P)
        t = scalar_mul(L, pt)
        if not point_equal(scalar_mul(4, t), IDENTITY):
            return t
    raise RuntimeError("no order-8 point found")
