#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""YapChain independent test-vector generator (M1.1).

Uses only the Python standard library (hashlib for SHA-2) and the reference
modules in this directory. It shares no code with the C++ implementation.

Usage: python3 -I tools/refgen/refgen.py --out test-vectors
"""
import argparse
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bech32m_ref as b32  # noqa: E402
import ed25519_ref as ed  # noqa: E402
import ycs_ref as ycs  # noqa: E402

U64_MAX = 2**64 - 1

HASH_TAGS = [
    "YAP/addr/v1",
    "YAP/sys-account/v1",
    "YAP/txid/v1",
    "YAP/state-key/v1",
    "YAP/state-value/v1",
    "YAP/smt/leaf/v1",
    "YAP/smt/node/v1",
    "YAP/genesis/v1",
    "YAP/nullifier/v1",
    "YAP/devblock/v1",
]

SIGN_PREFIXES = {
    "tx": b"YAPCHAIN/TX/V1\x00",
    "attestation": b"YAPCHAIN/ATTESTATION/V1\x00",
    "devblock": b"YAPCHAIN/DEVBLOCK/V1\x00",
}

SYSTEM_ACCOUNTS = ["treasury", "rewards-pool"]


def hx(b):
    return bytes(b).hex()


def tagged_hash(tag, msg):
    t = tag.encode("ascii")
    assert 1 <= len(t) <= 255
    return hashlib.sha256(bytes([len(t)]) + t + msg).digest()


def seed(i):
    return hashlib.sha256(b"yapchain/refgen/seed/" + str(i).encode()).digest()


def account_id(pub):
    return tagged_hash("YAP/addr/v1", b"\x00" + pub)


# --------------------------------------------------------------------------- sha256
def gen_sha256():
    cases = [
        ("empty", b""),
        ("abc", b"abc"),
        ("nist_448", b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
        ("nist_896", b"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                     b"hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
        ("bytes_0_to_255", bytes(range(256))),
    ]
    out = [{"name": n, "msg": hx(m), "digest": hashlib.sha256(m).hexdigest()} for n, m in cases]
    # Known NIST answers guard against a broken host hashlib.
    known = {
        "empty": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "abc": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "nist_448": "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
    }
    for v in out:
        if v["name"] in known:
            assert v["digest"] == known[v["name"]], v["name"]
    million = hashlib.sha256(b"a" * 1_000_000).hexdigest()
    assert million == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"
    out.append({"name": "million_a", "repeat_byte": "61", "repeat_count": 1_000_000, "digest": million})
    return out


# --------------------------------------------------------------------------- tagged hash
def gen_tagged_hash():
    msgs = [b"", b"abc", bytes(range(64))]
    hashes = []
    for tag in HASH_TAGS + ["x", "a" * 255]:
        for m in msgs:
            hashes.append({"tag": tag, "msg": hx(m), "digest": hx(tagged_hash(tag, m))})
    prefixes = [{"name": k, "bytes": hx(v)} for k, v in SIGN_PREFIXES.items()]
    return {"hashes": hashes, "sign_prefixes": prefixes}


# --------------------------------------------------------------------------- arithmetic
def gen_arith():
    m = U64_MAX
    pairs = [(0, 0), (1, 2), (m, 0), (m, 1), (m - 1, 1), (2**63, 2**63), (2**32, 2**32),
             (2**32 - 1, 2**32 + 1), (m, m), (12345678901234567, 89)]
    add, sub, mul = [], [], []
    for a, b in pairs:
        for x, y in ((a, b), (b, a)):
            add.append({"a": str(x), "b": str(y), "r": str(x + y) if x + y <= m else None})
            sub.append({"a": str(x), "b": str(y), "r": str(x - y) if x >= y else None})
            mul.append({"a": str(x), "b": str(y), "r": str(x * y) if x * y <= m else None})
    triples = [(0, 5, 1), (1, 1, 0), (m, m, m), (m, m, 1), (m, 5000, 10000), (2_000_000, 6000, 10000),
               (1_200_000, 10, 30), (m, 2, 3), (m, 3, 2), (10**15, 10**4, 10**4), (7, 3, 2), (2**63, 4, 2)]
    md = [{"a": str(a), "b": str(b), "c": str(c),
           "r": (str(a * b // c) if c != 0 and a * b // c <= m else None)} for a, b, c in triples]
    return {"checked_add": add, "checked_sub": sub, "checked_mul": mul, "mul_div_floor": md}


# --------------------------------------------------------------------------- ed25519
RFC8032 = [
    {
        "name": "rfc8032_test1",
        "seed": "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
        "pub": "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        "msg": "",
        "sig": "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555"
               "fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
    },
    {
        "name": "rfc8032_test2",
        "seed": "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
        "pub": "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        "msg": "72",
        "sig": "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
               "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
    },
    {
        "name": "rfc8032_test3",
        "seed": "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
        "pub": "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
        "msg": "af82",
        "sig": "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
               "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a",
    },
]


def _find_msg(pred, base):
    for i in range(10_000):
        m = base + i.to_bytes(4, "little")
        if pred(m):
            return m
    raise RuntimeError("no message found")


def gen_ed25519():
    for v in RFC8032:
        s = bytes.fromhex(v["seed"])
        assert hx(ed.public_key(s)) == v["pub"], v["name"]
        assert hx(ed.sign(s, bytes.fromhex(v["msg"]))) == v["sig"], v["name"]
        assert ed.verify(bytes.fromhex(v["pub"]), bytes.fromhex(v["msg"]), bytes.fromhex(v["sig"]))

    det = []
    for i in range(4):
        sd = seed(i)
        pub = ed.public_key(sd)
        for name, prefix in [("raw", b"")] + list(SIGN_PREFIXES.items()):
            msg = prefix + b"yap message " + bytes([i]) * i
            det.append({"seed": hx(sd), "pub": hx(pub), "msg": hx(msg), "sig": hx(ed.sign(sd, msg)),
                        "prefix": name})

    cases = []

    def add(name, pub, msg, sig, note):
        cases.append({"name": name, "pub": hx(pub), "msg": hx(msg), "sig": hx(sig),
                      "valid": ed.verify(pub, msg, sig), "note": note})

    sd = seed(100)
    pub = ed.public_key(sd)
    a_scalar, _ = ed.expand_seed(sd)
    msg = b"YapChain edge-case message"
    good = ed.sign(sd, msg)
    add("valid_baseline", pub, msg, good, "reference signature")
    add("wrong_message", pub, msg + b"!", good, "message changed")
    add("wrong_key", ed.public_key(seed(101)), msg, good, "different public key")
    flipped = bytearray(good)
    flipped[40] ^= 0x01
    add("flipped_s_bit", pub, msg, bytes(flipped), "bit flip in S")

    s_val = int.from_bytes(good[32:], "little")
    add("s_plus_l", pub, msg, good[:32] + int.to_bytes(s_val + ed.L, 32, "little"),
        "non-canonical S = S + L; equation holds modulo L, must be rejected by step 1")
    add("s_equals_l", pub, msg, good[:32] + int.to_bytes(ed.L, 32, "little"), "S = L")
    add("s_all_ff", pub, msg, good[:32] + b"\xff" * 32, "S = 2^256 - 1")

    base_enc = ed.compress(ed.BASE)
    one_le = int.to_bytes(1, 32, "little")

    # Small-order A with an otherwise valid equation ([1]B = B + [k]A with A = identity).
    identity_enc = int.to_bytes(1, 32, "little")
    add("a_identity", identity_enc, msg, base_enc + one_le,
        "A = identity (small order); equation holds; rejected by step 4")
    t8 = ed.torsion_point_of_order_8()
    t8_enc = ed.compress(t8)
    m8 = _find_msg(lambda m: ed.sha512_int(base_enc, t8_enc, m) % ed.L % 8 == 0, b"order8-")
    add("a_order_8", t8_enc, m8, base_enc + one_le,
        "A of order 8, k = 0 mod 8 so equation holds; rejected by step 4")
    a_neg_one = int.to_bytes(ed.P - 1, 32, "little")
    add("a_order_2", a_neg_one, msg, base_enc + one_le, "A = (0, -1), order 2")
    a_identity_signbit = int.to_bytes(1 | (1 << 255), 32, "little")
    add("a_identity_sign_bit", a_identity_signbit, msg, base_enc + one_le,
        "y = 1 with sign bit set (non-canonical identity)")

    # Non-canonical A encodings (y >= p).
    a_p_plus_1 = int.to_bytes(ed.P + 1, 32, "little")
    add("a_y_eq_p_plus_1", a_p_plus_1, msg, base_enc + one_le, "y = p + 1 (non-canonical 1)")
    for y in range(2, 19):
        if ed.recover_x(y, 0) is not None:
            enc = int.to_bytes(ed.P + y, 32, "little")
            add("a_noncanonical_y", enc, msg, good, f"y = p + {y}: non-canonical encoding of a valid point")
            break
    for y in range(2, 1000):
        if ed.recover_x(y, 0) is None:
            add("a_not_on_curve", int.to_bytes(y, 32, "little"), msg, good, f"y = {y}: no x exists")
            break

    # Small-order R with a valid equation: R = identity, S = k*a mod L.
    k_id = ed.sha512_int(identity_enc, pub, msg) % ed.L
    add("r_identity", pub, msg, identity_enc + int.to_bytes(k_id * a_scalar % ed.L, 32, "little"),
        "R = identity (small order), equation holds; rejected by step 2")
    r_p_plus_1 = int.to_bytes(ed.P + 1, 32, "little")
    k_np = ed.sha512_int(r_p_plus_1, pub, msg) % ed.L
    add("r_noncanonical_identity", pub, msg, r_p_plus_1 + int.to_bytes(k_np * a_scalar % ed.L, 32, "little"),
        "R = p + 1 (non-canonical identity)")
    r_flip = bytearray(good)
    r_flip[31] ^= 0x80
    add("r_sign_bit_flipped", pub, msg, bytes(r_flip), "sign bit of R flipped")

    # Mixed-order public key A' = A + T8 (not in SMALL_Y). Cofactorless equation holds iff k = 0 mod 8.
    a_pt = ed.decompress_canonical(pub)
    mixed = ed.compress(ed.point_add(a_pt, t8))
    assert ed.y_raw(mixed) not in ed.SMALL_Y

    def sign_with(a_enc, m):
        _, prefix = ed.expand_seed(sd)
        r = ed.sha512_int(prefix, m) % ed.L
        rs = ed.compress(ed.scalar_mul(r, ed.BASE))
        k = ed.sha512_int(rs, a_enc, m) % ed.L
        return rs + int.to_bytes((r + k * a_scalar) % ed.L, 32, "little"), k

    def k_for(m):
        return sign_with(mixed, m)[1]

    m_ok = _find_msg(lambda m: k_for(m) % 8 == 0, b"mixed-ok-")
    m_bad = _find_msg(lambda m: k_for(m) % 8 != 0, b"mixed-bad-")
    add("mixed_order_a_k_div_8", mixed, m_ok, sign_with(mixed, m_ok)[0],
        "mixed-order A, k = 0 mod 8: cofactorless equation holds -> accepted")
    add("mixed_order_a_k_not_div_8", mixed, m_bad, sign_with(mixed, m_bad)[0],
        "mixed-order A, k != 0 mod 8: only a cofactored verifier would accept")

    expected = {
        "valid_baseline": True, "wrong_message": False, "wrong_key": False, "flipped_s_bit": False,
        "s_plus_l": False, "s_equals_l": False, "s_all_ff": False, "a_identity": False, "a_order_8": False,
        "a_order_2": False, "a_identity_sign_bit": False, "a_y_eq_p_plus_1": False, "a_noncanonical_y": False,
        "a_not_on_curve": False, "r_identity": False, "r_noncanonical_identity": False,
        "r_sign_bit_flipped": False, "mixed_order_a_k_div_8": True, "mixed_order_a_k_not_div_8": False,
    }
    for c in cases:
        assert c["valid"] == expected[c["name"]], c["name"]
    return {"rfc8032": RFC8032, "deterministic": det, "verify": cases,
            "validator_keys": gen_validator_keys(pub, mixed, t8)}


def gen_validator_keys(pub, mixed, t8):
    """docs/protocol.md section 5.7 (ADR-0020): validator consensus public keys."""
    keys = []

    def add(name, enc, note):
        keys.append({"name": name, "pub": hx(enc), "valid": ed.is_valid_validator_key(enc), "note": note})

    for i in range(3):
        add(f"generated_{i}", ed.public_key(seed(200 + i)), "public key derived from a seed")
    add("base_point", ed.compress(ed.BASE), "generator B")
    add("verify_baseline_key", pub, "key of ed25519 verify vector valid_baseline")
    add("identity", int.to_bytes(1, 32, "little"), "neutral element, order 1")
    add("identity_sign_bit", int.to_bytes(1 | (1 << 255), 32, "little"), "y = 1 with sign bit (x = -0)")
    add("order_2", int.to_bytes(ed.P - 1, 32, "little"), "(0, -1)")
    add("order_4", int.to_bytes(0, 32, "little"), "y = 0")
    add("order_4_sign_bit", int.to_bytes(1 << 255, 32, "little"), "y = 0, other x")
    t8_enc = ed.compress(t8)
    add("order_8", t8_enc, "torsion point of order 8")
    add("order_8_negated", ed.compress(ed.point_neg(t8)), "negated torsion point of order 8")
    add("y_eq_p", int.to_bytes(ed.P, 32, "little"), "non-canonical encoding of y = 0")
    add("y_eq_p_plus_1", int.to_bytes(ed.P + 1, 32, "little"), "non-canonical encoding of y = 1")
    for y in range(2, 19):
        if ed.recover_x(y, 0) is not None:
            add("noncanonical_valid_point", int.to_bytes(ed.P + y, 32, "little"),
                f"y = p + {y}: non-canonical encoding of a curve point")
            break
    for y in range(2, 1000):
        if ed.recover_x(y, 0) is None:
            add("not_on_curve", int.to_bytes(y, 32, "little"), f"y = {y}: no x exists")
            break
    add("mixed_order_t8", mixed, "A + T8: accepted by signature verification (section 5.2), "
        "rejected here because it is outside the prime-order subgroup")
    a_pt = ed.decompress_canonical(pub)
    t4 = ed.scalar_mul(2, t8)
    t2 = ed.scalar_mul(4, t8)
    add("mixed_order_t4", ed.compress(ed.point_add(a_pt, t4)), "A + T4")
    add("mixed_order_t2", ed.compress(ed.point_add(a_pt, t2)), "A + T2")

    expected_valid = {"generated_0", "generated_1", "generated_2", "base_point", "verify_baseline_key"}
    for k in keys:
        assert k["valid"] == (k["name"] in expected_valid), k["name"]
    # The tightening is real: the mixed-order key is a valid signature-verification key.
    assert ed.y_raw(mixed) not in ed.SMALL_Y and ed.decompress_canonical(mixed) is not None
    return keys


# --------------------------------------------------------------------------- bech32m
BIP350_VALID = [
    "A1LQFN3A",
    "a1lqfn3a",
    "an83characterlonghumanreadablepartthatcontainsthetheexcludedcharactersbioandnumber11sg7hg6",
    "abcdef1l7aum6echk45nj3s0wdvt2fg8x9yrzpqzd3ryx",
    "11llllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllludsr8",
    "split1checkupstagehandshakeupstreamerranterredcaperredlc445v",
    "?1v759aa",
]
BIP350_INVALID = [
    (b"\x201xj0phk", "invalid_character"),
    (b"\x7f1g6xzxy", "invalid_character"),
    (b"\x801vctc34", "invalid_character"),
    (b"an84characterslonghumanreadablepartthatcontainsthetheexcludedcharactersbioandnumber11d6pts4",
     "invalid_length"),
    (b"qyrz8wqd2c9m", "missing_separator"),
    (b"1qyrz8wqd2c9m", "invalid_hrp"),
    (b"y1b0jsk6g", "invalid_character"),
    (b"lt1igcx5c0", "invalid_character"),
    (b"in1muywd", "data_too_short"),
    (b"mm1crxm3i", "invalid_character"),
    (b"au1s5cgom", "invalid_character"),
    (b"M1VUXWEZ", "invalid_checksum"),
    (b"16plkw9", "invalid_hrp"),
    (b"1p2gdwpf", "invalid_hrp"),
    # Valid Bech32 (BIP-173) strings must be rejected by a Bech32m decoder.
    (b"A12UEL5L", "invalid_checksum"),
    (b"abcdef1qpzry9x8gf2tvdw0s3jn54khce6mua7lmqqqxw", "invalid_checksum"),
    (b"aBcdef1l7aum6echk45nj3s0wdvt2fg8x9yrzpqzd3ryx", "mixed_case"),
]


def gen_bech32m():
    valid = []
    for s in BIP350_VALID:
        hrp, data = b32.decode(s.encode())
        assert b32.encode(hrp, data) == s.lower()
        valid.append({"input": hx(s.encode()), "text": s, "hrp": hrp, "data": data, "canonical": s.lower()})
    invalid = []
    for raw, kind in BIP350_INVALID:
        try:
            b32.decode(raw)
            raise AssertionError(f"accepted invalid {raw!r}")
        except ValueError as e:
            assert str(e) == kind, (raw, str(e), kind)
        invalid.append({"input": hx(raw), "error": kind})
    return {"valid": valid, "invalid": invalid}


# --------------------------------------------------------------------------- addresses
def gen_addresses():
    hrp = "tyap"
    keys = []
    for i in range(6):
        sd = seed(i)
        pub = ed.public_key(sd)
        aid = account_id(pub)
        addr = b32.encode_address(hrp, aid)
        assert len(addr) == 64
        assert b32.decode_address(addr.encode(), hrp) == aid
        keys.append({"seed": hx(sd), "pub": hx(pub), "account_id": hx(aid), "address": addr})
    system = []
    for name in SYSTEM_ACCOUNTS:
        aid = tagged_hash("YAP/sys-account/v1", name.encode())
        system.append({"name": name, "account_id": hx(aid), "address": b32.encode_address(hrp, aid)})

    aid = bytes.fromhex(keys[0]["account_id"])
    good = keys[0]["address"]
    five = [0] + b32.convertbits(aid, 8, 5, True)
    cases = []

    def case(name, text, expect_hrp="tyap"):
        raw = text.encode() if isinstance(text, str) else text
        entry = {"name": name, "input": hx(raw), "expected_hrp": expect_hrp}
        try:
            entry["account_id"] = hx(b32.decode_address(raw, expect_hrp))
            entry["ok"] = True
        except ValueError as e:
            entry["ok"] = False
            entry["error"] = str(e)
        cases.append(entry)
        return entry

    assert case("canonical", good)["ok"]
    assert case("uppercase", good.upper())["ok"]
    assert case("mixed_case", good[:10] + good[10:].upper())["error"] == "bech32"
    assert case("mainnet_hrp_rejected_on_testnet", b32.encode_address("yap", aid))["error"] == "wrong_hrp"
    assert case("mainnet_hrp_on_mainnet", b32.encode_address("yap", aid), "yap")["ok"]
    assert case("version_1", b32.encode(hrp, [1] + five[1:]))["error"] == "wrong_version"
    assert case("bech32_checksum", b32.encode(hrp, five, b32.BECH32_CONST))["error"] == "bech32"
    assert case("payload_31_bytes", b32.encode(hrp, [0] + b32.convertbits(aid[:31], 8, 5, True)))["error"] \
        == "wrong_length"
    assert case("payload_33_bytes", b32.encode(hrp, [0] + b32.convertbits(aid + b"\x00", 8, 5, True)))["error"] \
        == "wrong_length"
    padded = list(five)
    padded[-1] |= 0x01
    assert case("non_zero_padding", b32.encode(hrp, padded))["error"] == "non_zero_padding"
    assert case("empty_data", b32.encode(hrp, []))["error"] == "wrong_length"
    corrupted = good[:20] + ("q" if good[20] != "q" else "p") + good[21:]
    assert case("corrupted_char", corrupted)["error"] == "bech32"
    return {"hrp": hrp, "keys": keys, "system_accounts": system, "decode": cases}


# --------------------------------------------------------------------------- main
def write(out_dir, name, obj):
    path = os.path.join(out_dir, name)
    doc = {"generator": "tools/refgen/refgen.py", "version": 1, "vectors": obj}
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1, sort_keys=True, ensure_ascii=True)
        f.write("\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    write(args.out, "sha256.json", gen_sha256())
    write(args.out, "tagged_hash.json", gen_tagged_hash())
    write(args.out, "arith.json", gen_arith())
    write(args.out, "ycs.json", ycs.gen_vectors())
    write(args.out, "ed25519.json", gen_ed25519())
    write(args.out, "bech32m.json", gen_bech32m())
    write(args.out, "address.json", gen_addresses())
    return 0


if __name__ == "__main__":
    sys.exit(main())
