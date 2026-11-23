#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""YapChain independent test-vector generator (M1.1).

Uses only the Python standard library (hashlib for SHA-2) and the reference
modules in this directory. It shares no code with the C++ implementation.

Usage: python3 -I tools/refgen/refgen.py --out test-vectors
"""
import argparse
import random
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bech32m_ref as b32  # noqa: E402
import ed25519_ref as ed  # noqa: E402
import economics_ref as econ  # noqa: E402
import exec_ref  # noqa: E402
import merkle_ref  # noqa: E402
import state_ref  # noqa: E402
import tx_ref  # noqa: E402
from ycs_ref import DecodeError  # noqa: E402
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


# --------------------------------------------------------------------------- transactions
CHAIN_ID = "yapchain-testnet-1"


def _json_body(body):
    """JSON form of a body used by the C++ vector test to rebuild the transaction."""
    p = body["payload"]
    out = {"version": body["version"], "chain_id": body["chain_id"], "nonce": str(body["nonce"]),
           "fee": str(body["fee"]), "expires_at_height": str(body["expires_at_height"]), "memo": hx(body["memo"])}
    if p["type"] == "transfer":
        out["payload"] = {"type": "transfer", "to": hx(p["to"]), "amount": str(p["amount"])}
    elif p["type"] == "burn":
        out["payload"] = {"type": "burn", "amount": str(p["amount"])}
    else:
        a = p["attestation"]
        out["payload"] = {"type": "claim", "issuer_signature": hx(p["issuer_signature"]), "attestation": {
            "version": a["version"], "chain_id": a["chain_id"], "issuer_id": a["issuer_id"], "key_id": a["key_id"],
            "achievement_id": a["achievement_id"], "subject": hx(a["subject"]), "claim_id": hx(a["claim_id"]),
            "evidence_hash": hx(a["evidence_hash"]), "issued_at_s": str(a["issued_at_s"]),
            "expires_at_s": str(a["expires_at_s"])}}
    return out


def make_body(seed_bytes, payload, nonce=0, fee=1000, expires=100, memo=b"", chain_id=CHAIN_ID, version=1):
    return {"version": version, "chain_id": chain_id, "sender_pubkey": ed.public_key(seed_bytes), "nonce": nonce,
            "fee": fee, "expires_at_height": expires, "memo": memo, "payload": payload}


def gen_tx():
    s0, s1, issuer = seed(0), seed(1), seed(50)
    to = account_id(ed.public_key(s1))
    att = {"version": 1, "chain_id": CHAIN_ID, "issuer_id": 7, "key_id": 0, "achievement_id": 42,
           "subject": account_id(ed.public_key(s0)), "claim_id": bytes(range(16)), "evidence_hash": b"\x00" * 32,
           "issued_at_s": 1767225600, "expires_at_s": 1767225600 + 86400}
    claim = {"type": "claim", "attestation": att, "issuer_signature": tx_ref.sign_attestation(att, issuer)}
    cases = [
        ("transfer", make_body(s0, {"type": "transfer", "to": to, "amount": 1_500_000})),
        ("transfer_max_values", make_body(s0, {"type": "transfer", "to": to, "amount": 2**64 - 1},
                                          nonce=2**64 - 1, fee=2**64 - 1, expires=2**64 - 1, memo=b"\xff" * 1024)),
        ("burn_with_memo", make_body(s1, {"type": "burn", "amount": 7}, nonce=3, memo="мем 🐸".encode())),
        ("claim", make_body(s0, claim, nonce=1)),
    ]
    encode = []
    for name, body in cases:
        signer = s0 if body["sender_pubkey"] == ed.public_key(s0) else s1
        txb = tx_ref.sign_tx(body, signer)
        assert tx_ref.decode_tx(txb) == (body, txb[-64:])
        encode.append({"name": name, "seed": hx(signer), "body": _json_body(body),
                       "sender_account_id": hx(account_id(body["sender_pubkey"])),
                       "body_hex": hx(tx_ref.encode_body(body)), "signature": hx(txb[-64:]),
                       "tx_hex": hx(txb), "tx_id": hx(tx_ref.tx_id(txb))})
    encode[3]["issuer_seed"] = hx(issuer)

    good = bytes.fromhex(encode[0]["tx_hex"])
    body0 = make_body(s0, {"type": "transfer", "to": to, "amount": 1})

    def raw(body, sig=b"\x00" * 64):
        return tx_ref.encode_body(body) + sig

    def with_tag(tag):
        b = tx_ref.encode_body(make_body(s0, {"type": "burn", "amount": 1}))
        return b[:-9] + bytes([tag]) + b[-8:] + b"\x00" * 64

    invalid = [
        ("empty", b""),
        ("truncated_signature", good[:-1]),
        ("trailing_byte", good + b"\x00"),
        ("payload_tag_3_reserved", with_tag(3)),
        ("payload_tag_0x10_admin", with_tag(0x10)),
        ("payload_tag_0xff", with_tag(0xFF)),
        ("chain_id_uppercase", raw(dict(body0, chain_id="YAPCHAIN"))),
        ("chain_id_leading_dash", raw(dict(body0, chain_id="-yap"))),
        ("chain_id_empty", raw(dict(body0, chain_id=""))),
        ("chain_id_33_bytes", raw(dict(body0, chain_id="a" * 33))),
        ("memo_1025_bytes", raw(dict(body0, memo=b"m" * 1025))),
        ("attestation_bad_chain_id", raw(make_body(s0, dict(claim, attestation=dict(att, chain_id="Bad"))))),
    ]
    decode_invalid = []
    for name, data in invalid:
        try:
            tx_ref.decode_tx(data)
            raise AssertionError("accepted " + name)
        except DecodeError as e:
            decode_invalid.append({"name": name, "hex": hx(data), "error": str(e)})
    return {"chain_id": CHAIN_ID, "encode": encode, "decode_invalid": decode_invalid}


# --------------------------------------------------------------------------- validation
def gen_validation():
    params = tx_ref.TESTNET_PARAMS
    H = 10
    s_a, s_b, s_new, s_max, s_issuer = seed(0), seed(1), seed(2), seed(3), seed(50)
    a_id, b_id = account_id(ed.public_key(s_a)), account_id(ed.public_key(s_b))
    max_id = account_id(ed.public_key(s_max))
    accounts = {a_id: {"balance": 10_000_000, "nonce": 3}, b_id: {"balance": 2_000, "nonce": 0},
                max_id: {"balance": 10**12, "nonce": tx_ref.U64_MAX}}

    def body(sd=s_a, payload=None, **kw):
        fields = dict(nonce=3 if sd == s_a else 0, fee=1000, expires=20)
        fields.update(kw)
        return make_body(sd, payload or {"type": "transfer", "to": b_id, "amount": 1000}, **fields)

    def signed(b, sd=None):
        sd = sd or next(x for x in (s_a, s_b, s_new, s_max) if ed.public_key(x) == b["sender_pubkey"])
        return tx_ref.sign_tx(b, sd)

    def bad_sig(b):
        return tx_ref.encode_body(b) + tx_ref.sign_tx(dict(b, sender_pubkey=ed.public_key(s_b)), s_b)[-64:]

    def claim(**over):
        att = {"version": 1, "chain_id": CHAIN_ID, "issuer_id": 1, "key_id": 0, "achievement_id": 1,
               "subject": a_id, "claim_id": b"\x01" * 16, "evidence_hash": b"\x00" * 32,
               "issued_at_s": 1000, "expires_at_s": 2000}
        att.update(over)
        return {"type": "claim", "attestation": att, "issuer_signature": tx_ref.sign_attestation(att, s_issuer)}

    garbage_big = b"\x00" * 4097
    cases = [
        ("ok_transfer", signed(body()), "OK"),
        ("ok_burn", signed(body(payload={"type": "burn", "amount": 5})), "OK"),
        ("ok_memo_at_limit", signed(body(memo=b"m" * 128)), "OK"),
        ("ok_expires_equals_height", signed(body(expires=H)), "OK"),
        ("ok_ttl_at_limit", signed(body(expires=H + 1800)), "OK"),
        ("ok_exact_balance", signed(body(s_b, {"type": "transfer", "to": a_id, "amount": 1000})), "OK"),
        ("S1_too_large", garbage_big, "ERR_TOO_LARGE"),
        ("S2_garbage", b"\x01\x02\x03", "ERR_DECODE"),
        ("S2_unknown_payload_tag", tx_ref.encode_body(body(payload={"type": "burn", "amount": 1}))[:-9]
         + b"\x07" + b"\x00" * 72, "ERR_DECODE"),
        ("S3_version_2", signed(body(version=2)), "ERR_VERSION"),
        ("S3_before_S4", signed(body(version=2, chain_id="other-chain")), "ERR_VERSION"),
        ("S4_wrong_chain", signed(body(chain_id="yapchain-testnet-2")), "ERR_CHAIN_ID"),
        ("S5_memo_129", signed(body(memo=b"m" * 129)), "ERR_MEMO"),
        ("S6_fee_999", signed(body(fee=999)), "ERR_FEE"),
        ("S6_before_S11", bad_sig(body(fee=999)), "ERR_FEE"),
        ("S7_expires_zero", signed(body(expires=0)), "ERR_EXPIRED"),
        ("S8_amount_zero", signed(body(payload={"type": "transfer", "to": b_id, "amount": 0})), "ERR_PAYLOAD"),
        ("S8_self_transfer", signed(body(payload={"type": "transfer", "to": a_id, "amount": 1})), "ERR_PAYLOAD"),
        ("S9_burn_zero", signed(body(payload={"type": "burn", "amount": 0})), "ERR_PAYLOAD"),
        ("S10_subject_mismatch", signed(body(payload=claim(subject=b_id))), "ERR_ATTESTATION"),
        ("S10_chain_mismatch", signed(body(payload=claim(chain_id="other-chain"))), "ERR_ATTESTATION"),
        ("S10_version_2", signed(body(payload=claim(version=2))), "ERR_ATTESTATION"),
        ("S10_issued_not_before_expiry", signed(body(payload=claim(issued_at_s=2000))), "ERR_ATTESTATION"),
        ("S10_validity_too_long", signed(body(payload=claim(expires_at_s=1000 + 604801))), "ERR_ATTESTATION"),
        ("S10_before_S11", bad_sig(body(payload=claim(version=2))), "ERR_ATTESTATION"),
        ("S11_wrong_signature", bad_sig(body()), "ERR_SIGNATURE"),
        ("S11_before_T3", bad_sig(body(nonce=99)), "ERR_SIGNATURE"),
        ("T1_expired", signed(body(expires=H - 1)), "ERR_EXPIRED"),
        ("T1_before_T3", signed(body(expires=H - 1, nonce=99)), "ERR_EXPIRED"),
        ("T2_ttl_too_long", signed(body(expires=H + 1801)), "ERR_TTL_TOO_LONG"),
        ("T2_expires_u64_max", signed(body(expires=tx_ref.U64_MAX)), "ERR_TTL_TOO_LONG"),
        ("T3_nonce_low", signed(body(nonce=2)), "ERR_NONCE"),
        ("T3_nonce_high", signed(body(nonce=4)), "ERR_NONCE"),
        ("T3_new_account_nonce_1", signed(body(s_new, nonce=1)), "ERR_NONCE"),
        ("T3_before_T6", signed(body(nonce=4, payload={"type": "transfer", "to": b_id, "amount": 10**15})),
         "ERR_NONCE"),
        ("T4_nonce_u64_max", signed(body(s_max, nonce=tx_ref.U64_MAX)), "ERR_OVERFLOW"),
        ("T5_amount_plus_fee_overflow",
         signed(body(payload={"type": "transfer", "to": b_id, "amount": tx_ref.U64_MAX})), "ERR_OVERFLOW"),
        ("T6_new_account_no_funds", signed(body(s_new, nonce=0)), "ERR_INSUFFICIENT_FUNDS"),
        ("T6_one_yip_short", signed(body(s_b, {"type": "transfer", "to": a_id, "amount": 1001})),
         "ERR_INSUFFICIENT_FUNDS"),
        ("T6_burn_too_much", signed(body(payload={"type": "burn", "amount": 9_999_001})), "ERR_INSUFFICIENT_FUNDS"),
        ("T7_claim_unsupported_until_m3", signed(body(payload=claim())), "ERR_UNSUPPORTED"),
    ]
    out = []
    for name, data, expected in cases:
        got = tx_ref.validate(data, H, CHAIN_ID, accounts, params)
        assert got == expected, (name, got, expected)
        out.append({"name": name, "tx_hex": hx(data), "code": expected})
    return {"chain_id": CHAIN_ID, "height": str(H),
            "params": {k: str(v) for k, v in params.items()},
            "accounts": [{"account_id": hx(k), "balance": str(v["balance"]), "nonce": str(v["nonce"])}
                         for k, v in sorted(accounts.items())],
            "cases": out}


# --------------------------------------------------------------------------- economics
def gen_economics():
    base = dict(tx_ref.TESTNET_PARAMS)
    vals = []
    for i in range(4):
        pub = ed.public_key(seed(300 + i))
        addr = hashlib.sha256(pub).digest()[:20]
        vals.append({"address": addr, "pub": pub, "reward_account": account_id(ed.public_key(seed(400 + i)))})
    by_addr = sorted(vals, key=lambda x: x["address"])

    def run(name, powers, flags, total_minted=0, **param_over):
        params = dict(base, **param_over)
        validators = {v["address"]: (pw, v["reward_account"]) for v, pw in zip(by_addr, powers)}
        commit = [(v["address"], pw, f) for v, pw, f in zip(by_addr, powers, flags) if f is not None]
        credits, minted = econ.emission(params, total_minted, commit, validators)
        return {"name": name,
                "params": {k: str(v) for k, v in params.items()},
                "total_minted_before": str(total_minted),
                "validators": [{"consensus_pubkey": hx(v["pub"]), "address": hx(v["address"]), "power": str(pw),
                                "reward_account": hx(v["reward_account"])} for v, pw in zip(by_addr, powers)],
                "last_commit": [{"address": hx(a), "power": str(pw), "flag": f} for a, pw, f in commit],
                "minted": str(minted),
                "credits": [{"account_id": hx(k), "amount": str(v)} for k, v in sorted(credits.items())]}

    m = base["max_issuance"]
    cases = [
        run("all_four_signed", [10] * 4, [2] * 4),
        run("three_of_four_signed_one_absent", [10] * 4, [2, 2, 1, 2]),
        run("nil_vote_gets_nothing", [10] * 4, [2, 3, 2, 2]),
        run("height_1_empty_commit", [10] * 4, [None] * 4),
        run("nobody_signed", [10] * 4, [1, 3, 1, 1]),
        run("unequal_power_remainder", [3, 7, 1, 2], [2, 2, 1, 1], block_reward=1_000_003),
        run("odd_split_bps", [5, 5, 5, 5], [2, 2, 2, 1], block_reward=999_999, reward_validators_bps=3333,
            reward_pool_bps=3333, reward_treasury_bps=3334),
        run("partial_last_reward", [10] * 4, [2] * 4, total_minted=m - 5),
        run("one_yip_left", [10] * 4, [2] * 4, total_minted=m - 1),
        run("cap_reached", [10] * 4, [2] * 4, total_minted=m),
    ]
    assert cases[-1]["minted"] == "0" and cases[-1]["credits"] == []
    assert cases[-3]["minted"] == "5"
    return {"cases": cases}


# --------------------------------------------------------------------------- merkle
def gen_merkle():
    def k(i):
        return int(i).to_bytes(32, "big")

    def vh(i):
        return hashlib.sha256(b"v" + str(i).encode()).digest()

    leaf_sets = [
        ("empty", []),
        ("single", [(k(5), vh(1))]),
        ("two_split_at_root", [(k(0), vh(1)), (bytes([0x80]) + b"\x00" * 31, vh(2))]),
        ("two_differ_in_last_bit", [(k(0), vh(1)), (k(1), vh(2))]),
        ("three_mixed", [(k(0), vh(1)), (k(1), vh(2)), (b"\xff" * 32, vh(3))]),
        ("all_ones_and_zero", [(b"\x00" * 32, vh(4)), (b"\xff" * 32, vh(5))]),
        ("sixteen_sequential", [(k(i), vh(i)) for i in range(16)]),
        ("hashed_keys", [(hashlib.sha256(str(i).encode()).digest(), vh(i)) for i in range(40)]),
    ]
    smt = [{"name": n, "leaves": [{"key": hx(a), "value_hash": hx(b)} for a, b in leaves],
            "root": hx(merkle_ref.smt_root(leaves))} for n, leaves in leaf_sets]
    entries = {bytes([1]) + bytes([i]) * 32: bytes([i]) * (i % 5 + 1) for i in range(25)}
    entries[b"\x00"] = b"chain-state"
    entries[b"\x02"] = b"params"
    state = {"entries": [{"raw_key": hx(a), "value": hx(b)} for a, b in sorted(entries.items())],
             "root": hx(merkle_ref.state_root(entries))}
    lists = [{"n": n, "root": hx(merkle_ref.rfc6962_root([bytes([i]) * (i + 1) for i in range(n)]))}
             for n in (0, 1, 2, 3, 4, 5, 7, 8, 9, 16, 17)]
    assert lists[0]["root"] == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    return {"smt": smt, "state": state, "rfc6962": lists}


# --------------------------------------------------------------------------- genesis
YAP = 1_000_000


def reference_genesis(params=None, extra_accounts=(), n_validators=4, fresh_reward_accounts=False):
    """Testnet-like genesis following economics.md §3 (amounts in yip)."""
    treasury = tagged_hash("YAP/sys-account/v1", b"treasury")
    pool = tagged_hash("YAP/sys-account/v1", b"rewards-pool")
    faucet = account_id(ed.public_key(seed(500)))
    accounts = {treasury: 80_000_000 * YAP, pool: 60_000_000 * YAP, faucet: 50_000_000 * YAP}
    validators = []
    for i in range(n_validators):
        operator = account_id(ed.public_key(seed(510 + i)))
        accounts[operator] = 2_500_000 * YAP
        reward = account_id(ed.public_key(seed(540 + i))) if fresh_reward_accounts else operator
        validators.append({"consensus_pubkey": ed.public_key(seed(520 + i)), "power": 10,
                           "reward_account": reward, "name": f"validator-{i + 1}"})
    for aid, bal in extra_accounts:
        accounts[aid] = accounts.get(aid, 0) + bal
    validators.sort(key=lambda v: state_ref.consensus_address(v["consensus_pubkey"]))
    return {
        "chain_id": CHAIN_ID, "genesis_time_ms": 1_767_225_600_000, "initial_height": 1,
        "params": dict(params or tx_ref.TESTNET_PARAMS),
        "accounts": [{"account_id": k, "balance": v} for k, v in sorted(accounts.items())],
        "validators": validators,
        "issuers": [{"issuer_id": 1, "name": "Mempassport", "status": 1, "epoch_budget": 1_000_000 * YAP,
                     "keys": [{"key_id": 0, "pubkey": ed.public_key(seed(530)), "status": 1},
                              {"key_id": 1, "pubkey": ed.public_key(seed(531)), "status": 2}]},
                    {"issuer_id": 7, "name": "Yapped", "status": 1, "epoch_budget": 10_000 * YAP,
                     "keys": [{"key_id": 3, "pubkey": ed.public_key(seed(532)), "status": 1}]}],
        "achievements": [{"issuer_id": 1, "achievement_id": 1, "reward": 100 * YAP, "policy": 1,
                          "max_per_epoch": 0, "status": 1},
                         {"issuer_id": 1, "achievement_id": 2, "reward": 5 * YAP, "policy": 2,
                          "max_per_epoch": 3, "status": 1},
                         {"issuer_id": 7, "achievement_id": 1, "reward": 1, "policy": 1,
                          "max_per_epoch": 0, "status": 2}],
    }


def genesis_text(g):
    return json.dumps(state_ref.to_json_obj(g), indent=1, ensure_ascii=True)


def genesis_entry(name, g):
    text = genesis_text(g)
    assert state_ref.parse_genesis_json(text) == g, name
    return {"name": name, "json": text, "genesis_hash": hx(state_ref.genesis_hash(g)),
            "ycs": hx(state_ref.encode_genesis(g)), "initial_state_root": hx(merkle_ref.state_root(
                state_ref.initial_state(g)))}


def gen_genesis():
    base = reference_genesis()
    minimal = dict(base, accounts=[], issuers=[], achievements=[], validators=base["validators"][:1])
    valid = [genesis_entry("testnet_reference", base), genesis_entry("minimal_one_validator", minimal)]

    obj = state_ref.to_json_obj(base)

    def mutate(fn):
        o = json.loads(json.dumps(obj))
        fn(o)
        return json.dumps(o)

    def setp(path, value):
        def f(o):
            cur = o
            for p in path[:-1]:
                cur = cur[p]
            cur[path[-1]] = value
        return mutate(f)

    def drop(path):
        def f(o):
            cur = o
            for p in path[:-1]:
                cur = cur[p]
            del cur[path[-1]]
        return mutate(f)

    vals = base["validators"]
    invalid_texts = [
        ("malformed_json", genesis_text(base)[:-2]),
        ("duplicate_key", genesis_text(base).replace('"chain_id"', '"chain_id": "x", "chain_id"', 1)),
        ("unknown_top_field", setp(["extra"], 1)),
        ("missing_params_field", drop(["params", "min_fee"])),
        ("unknown_params_field", setp(["params", "gas"], "1")),
        ("u64_as_number", setp(["genesis_time_ms"], 1767225600000)),
        ("u64_leading_zero", setp(["genesis_time_ms"], "01")),
        ("u64_plus_sign", setp(["genesis_time_ms"], "+1")),
        ("u64_overflow", setp(["genesis_time_ms"], "18446744073709551616")),
        ("u16_as_string", setp(["params", "fee_burn_bps"], "5000")),
        ("u16_negative", setp(["params", "fee_burn_bps"], -1)),
        ("u16_float", genesis_text(base).replace('"fee_burn_bps": 5000', '"fee_burn_bps": 5000.0')),
        ("u16_exponent", genesis_text(base).replace('"fee_burn_bps": 5000', '"fee_burn_bps": 5e3')),
        ("u16_out_of_range", setp(["params", "fee_burn_bps"], 65536)),
        ("u8_boolean", setp(["issuers", 0, "status"], True)),
        ("hex_uppercase", setp(["validators", 0, "consensus_pubkey"], vals[0]["consensus_pubkey"].hex().upper())),
        ("address_wrong_hrp", setp(["accounts", 0, "address"],
                                   bech32m_ref_encode("yap", base["accounts"][0]["account_id"]))),
        ("chain_id_invalid", setp(["chain_id"], "Yap")),
        ("initial_height_2", setp(["initial_height"], "2")),
        ("params_bps_sum", setp(["params", "reward_pool_bps"], 2999)),
        ("params_min_fee_zero", setp(["params", "min_fee"], "0")),
        ("accounts_unsorted", mutate(lambda o: o["accounts"].reverse())),
        ("accounts_duplicate", mutate(lambda o: o["accounts"].insert(1, o["accounts"][0]))),
        ("account_zero_balance", setp(["accounts", 0, "balance"], "0")),
        ("supply_above_max_issuance", setp(["params", "max_issuance"], "1000")),
        ("no_validators", setp(["validators"], [])),
        ("validators_unsorted", mutate(lambda o: o["validators"].reverse())),
        ("validator_power_zero", setp(["validators", 0, "power"], "0")),
        ("validator_total_power_above_2_60", setp(["validators", 0, "power"], str(2**60))),
        ("validator_small_order_key", setp(["validators", 0, "consensus_pubkey"], "01" + "00" * 31)),
        ("validator_empty_name", setp(["validators", 0, "name"], "")),
        ("validator_name_non_ascii", setp(["validators", 0, "name"], "валидатор")),
        ("issuer_no_keys", setp(["issuers", 0, "keys"], [])),
        ("issuer_bad_status", setp(["issuers", 0, "status"], 3)),
        ("issuer_keys_unsorted", mutate(lambda o: o["issuers"][0]["keys"].reverse())),
        ("issuers_unsorted", mutate(lambda o: o["issuers"].reverse())),
        ("achievement_unknown_issuer", setp(["achievements", 0, "issuer_id"], 99)),
        ("achievement_once_with_max", setp(["achievements", 0, "max_per_epoch"], 1)),
        ("achievement_per_epoch_zero", setp(["achievements", 1, "max_per_epoch"], 0)),
        ("achievement_bad_policy", setp(["achievements", 0, "policy"], 3)),
        ("achievement_zero_reward", setp(["achievements", 0, "reward"], "0")),
        ("achievements_unsorted", mutate(lambda o: o["achievements"].reverse())),
    ]
    invalid = []
    for name, text in invalid_texts:
        try:
            state_ref.parse_genesis_json(text)
            raise AssertionError("reference accepted invalid genesis " + name)
        except (ValueError, state_ref.JsonSchemaError):
            pass
        invalid.append({"name": name, "json": text})
    return {"valid": valid, "invalid": invalid}


def bech32m_ref_encode(hrp, aid):
    return b32.encode_address(hrp, aid)


# --------------------------------------------------------------------------- block execution
USERS = [seed(600 + i) for i in range(8)]


def uid(i):
    return account_id(ed.public_key(USERS[i]))


class Scenario:
    def __init__(self, name, genesis):
        self.name, self.g = name, genesis
        self.chain = exec_ref.Chain(genesis)
        self.genesis_root = self.chain.root()
        self.blocks = []
        self.addrs = sorted(self.chain.validators)

    def commit_votes(self, flags):
        return [(a, self.chain.validators[a]["power"], f) for a, f in zip(self.addrs, flags) if f is not None]

    def block(self, txs, flags=None):
        h = self.chain.cs["height"] + 1
        votes = [] if h == 1 else self.commit_votes(flags or [2] * len(self.addrs))
        t = self.g["genesis_time_ms"] + h * 2000
        codes, root = self.chain.execute_block(h, t, votes, txs)
        self.blocks.append({"height": str(h), "time_ms": str(t),
                            "last_commit": [{"address": hx(a), "power": str(p), "flag": f} for a, p, f in votes],
                            "txs": [hx(x) for x in txs], "codes": codes, "accept": all(c == "OK" for c in codes),
                            "state_root": hx(root), "chain_state": {k: str(v) for k, v in self.chain.cs.items()}})
        return codes

    def to_json(self):
        return {"name": self.name, "genesis": genesis_text(self.g), "genesis_root": hx(self.genesis_root),
                "blocks": self.blocks}


def tx(i, payload, nonce, fee=1000, expires=1_000, memo=b"", signer=None):
    body = make_body(USERS[i], payload, nonce=nonce, fee=fee, expires=expires, memo=memo)
    return tx_ref.sign_tx(body, USERS[signer if signer is not None else i]) if signer is None else \
        tx_ref.encode_body(body) + tx_ref.sign_tx(dict(body, sender_pubkey=ed.public_key(USERS[signer])),
                                                  USERS[signer])[-64:]


def transfer(i, to, amount, nonce, **kw):
    return tx(i, {"type": "transfer", "to": to, "amount": amount}, nonce, **kw)


def burn(i, amount, nonce, **kw):
    return tx(i, {"type": "burn", "amount": amount}, nonce, **kw)


def scenario_basics():
    g = reference_genesis(extra_accounts=[(uid(0), 10 * YAP), (uid(1), 3_000)])
    sc = Scenario("ledger_basics", g)
    t1 = transfer(0, uid(1), YAP, 0)
    att = {"version": 1, "chain_id": CHAIN_ID, "issuer_id": 1, "key_id": 0, "achievement_id": 1,
           "subject": uid(0), "claim_id": b"\x05" * 16, "evidence_hash": b"\x00" * 32,
           "issued_at_s": 1_767_225_600, "expires_at_s": 1_767_225_600 + 3600}
    claim = {"type": "claim", "attestation": att, "issuer_signature": tx_ref.sign_attestation(att, seed(530))}
    expect = [
        sc.block([t1, transfer(0, uid(2), 2 * YAP, 1), burn(1, 500, 0), transfer(0, uid(1), 1, 5),
                  transfer(0, uid(1), 1, 2, signer=3), transfer(0, uid(1), 1, 2, expires=0), t1]),
        sc.block([transfer(2, uid(4), 2 * YAP - 1000, 0), transfer(2, uid(4), 1, 1),
                  transfer(1, tagged_hash("YAP/sys-account/v1", b"treasury"), 100, 1)]),
        sc.block([b"\x00" * 4097, tx(0, claim, 2), transfer(0, uid(1), tx_ref.U64_MAX, 2),
                  transfer(0, uid(1), 1, 2, memo=b"m" * 129)], flags=[2, 2, 3, 2]),
        sc.block([], flags=[2, 1, 2, 1]),
        sc.block([transfer(1, uid(2), 5_000, 2), transfer(2, uid(0), 1, 1)]),
    ]
    assert expect[0] == ["OK", "OK", "OK", "ERR_NONCE", "ERR_SIGNATURE", "ERR_EXPIRED", "ERR_NONCE"], expect[0]
    assert expect[1] == ["OK", "ERR_INSUFFICIENT_FUNDS", "OK"], expect[1]
    assert expect[2] == ["ERR_TOO_LARGE", "ERR_UNSUPPORTED", "ERR_OVERFLOW", "ERR_MEMO"], expect[2]
    assert expect[3] == [] and expect[4] == ["OK", "OK"], expect[4]
    return sc


def scenario_emission_cap():
    supply_g = reference_genesis(extra_accounts=[(uid(0), 10 * YAP)])
    supply = sum(a["balance"] for a in supply_g["accounts"])
    params = dict(tx_ref.TESTNET_PARAMS, max_issuance=supply + 3_000_005)
    sc = Scenario("emission_cap", reference_genesis(params, extra_accounts=[(uid(0), 10 * YAP)]))
    sc.block([transfer(0, uid(1), 10, 0)])
    sc.block([transfer(0, uid(1), 10, 1)])
    sc.block([burn(0, 5 * YAP, 2)])                       # burns do not re-enable emission
    sc.block([transfer(0, uid(1), 10, 3)], flags=[2, 2, 2, 3])
    minted = [int(b["chain_state"]["total_minted"]) for b in sc.blocks]
    assert [y - x for x, y in zip([supply] + minted, minted)] == [2_000_000, 1_000_005, 0, 0]
    return sc


def scenario_zero_shares():
    params = dict(tx_ref.TESTNET_PARAMS, block_reward=3, fee_burn_bps=10000)
    sc = Scenario("zero_shares", reference_genesis(params, extra_accounts=[(uid(0), YAP)],
                                                   fresh_reward_accounts=True))
    sc.block([transfer(0, uid(1), 10, 0)])
    sc.block([transfer(0, uid(1), 10, 1)])
    sc.block([])
    for v in sc.chain.validators.values():
        assert v["reward_account"] not in sc.chain.accounts  # shares of 0 never created accounts
    return sc


def scenario_long_run():
    funded = [(uid(i), (i + 1) * 3 * YAP) for i in range(6)]
    sc = Scenario("long_run", reference_genesis(extra_accounts=funded))
    rng = random.Random(0x59415043)
    for _ in range(40):
        txs = []
        for _ in range(rng.randint(0, 9)):
            i = rng.randrange(8)
            acct = sc.chain.accounts.get(uid(i), {"balance": 0, "nonce": 0})
            nonce = acct["nonce"] + sum(1 for t in txs if tx_ref.decode_tx(t)[0]["sender_pubkey"] ==
                                        ed.public_key(USERS[i]))
            if rng.random() < 0.15:
                nonce += rng.choice([-1, 1, 3])
            nonce = max(nonce, 0)
            amount = rng.choice([1, 999, rng.randrange(1, max(2, acct["balance"] + 2 * YAP))])
            if rng.random() < 0.7:
                to = uid(rng.randrange(8))
                if to == uid(i):
                    to = uid((i + 1) % 8)
                txs.append(transfer(i, to, amount, nonce, fee=rng.choice([1000, 1000, 2500, 999])))
            else:
                txs.append(burn(i, amount, nonce))
        flags = [rng.choice([1, 2, 2, 2, 3]) for _ in range(4)]
        sc.block(txs, flags=flags)
    codes = [c for b in sc.blocks for c in b["codes"]]
    assert codes.count("OK") > 50 and len(set(codes)) >= 4, (codes.count("OK"), set(codes))
    return sc


def gen_blocks():
    return {"scenarios": [s.to_json() for s in (scenario_basics(), scenario_emission_cap(),
                                                 scenario_zero_shares(), scenario_long_run())]}


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
    write(args.out, "tx.json", gen_tx())
    write(args.out, "validation.json", gen_validation())
    write(args.out, "economics.json", gen_economics())
    write(args.out, "merkle.json", gen_merkle())
    write(args.out, "genesis.json", gen_genesis())
    write(args.out, "blocks.json", gen_blocks())
    return 0


if __name__ == "__main__":
    sys.exit(main())
