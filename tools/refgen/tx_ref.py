# SPDX-License-Identifier: Apache-2.0
"""Independent reference for transaction formats (docs/protocol.md §6.1, attestations.md §3.1).

Transactions and attestations are plain dicts:
  body = {version, chain_id, sender_pubkey(bytes), nonce, fee, expires_at_height, memo(bytes), payload}
  payload = {"type": "transfer", "to": bytes, "amount": int}
          | {"type": "burn", "amount": int}
          | {"type": "claim", "attestation": att, "issuer_signature": bytes}
  att = {version, chain_id, issuer_id, key_id, achievement_id, subject, claim_id, evidence_hash,
         issued_at_s, expires_at_s}
"""
import hashlib
import re

import ed25519_ref as ed
from ycs_ref import DecodeError, Reader

TX_PREFIX = b"YAPCHAIN/TX/V1\x00"
ATTESTATION_PREFIX = b"YAPCHAIN/ATTESTATION/V1\x00"
MEMO_CEILING = 1024
TAGS = {"transfer": 0, "burn": 1, "claim": 2}


def tagged_hash(tag, msg):
    t = tag.encode("ascii")
    return hashlib.sha256(bytes([len(t)]) + t + msg).digest()


def account_id(pub):
    return tagged_hash("YAP/addr/v1", b"\x00" + pub)


def valid_chain_id(s):
    return re.fullmatch(r"[a-z0-9-]{1,32}", s) is not None and not s.startswith("-") and not s.endswith("-")


def u(width, v):
    return int(v).to_bytes(width, "little")


def enc_bytes(b):
    return u(4, len(b)) + bytes(b)


def encode_attestation(a):
    return (u(2, a["version"]) + enc_bytes(a["chain_id"].encode()) + u(4, a["issuer_id"]) + u(1, a["key_id"])
            + u(4, a["achievement_id"]) + a["subject"] + a["claim_id"] + a["evidence_hash"]
            + u(8, a["issued_at_s"]) + u(8, a["expires_at_s"]))


def encode_body(b):
    out = (u(2, b["version"]) + enc_bytes(b["chain_id"].encode()) + b["sender_pubkey"] + u(8, b["nonce"])
           + u(8, b["fee"]) + u(8, b["expires_at_height"]) + enc_bytes(b["memo"]))
    p = b["payload"]
    out += bytes([TAGS[p["type"]]])
    if p["type"] == "transfer":
        out += p["to"] + u(8, p["amount"])
    elif p["type"] == "burn":
        out += u(8, p["amount"])
    else:
        out += encode_attestation(p["attestation"]) + p["issuer_signature"]
    return out


def sign_attestation(att, issuer_seed):
    return ed.sign(issuer_seed, ATTESTATION_PREFIX + encode_attestation(att))


def sign_tx(body, seed):
    assert body["sender_pubkey"] == ed.public_key(seed)
    return encode_body(body) + ed.sign(seed, TX_PREFIX + encode_body(body))


def tx_id(tx_bytes):
    return tagged_hash("YAP/txid/v1", tx_bytes)


def _chain_id(r):
    s = r.bytes_(32)
    try:
        text = s.decode("ascii")
    except UnicodeDecodeError:
        raise DecodeError("invalid_content")
    if not valid_chain_id(text):
        raise DecodeError("invalid_content")
    return text


def _attestation(r):
    return {"version": r.uint(2), "chain_id": _chain_id(r), "issuer_id": r.uint(4), "key_id": r.uint(1),
            "achievement_id": r.uint(4), "subject": r.take(32), "claim_id": r.take(16),
            "evidence_hash": r.take(32), "issued_at_s": r.uint(8), "expires_at_s": r.uint(8)}


def decode_tx(data):
    """Returns (body, signature) or raises DecodeError(kind)."""
    r = Reader(data)
    body = {"version": r.uint(2), "chain_id": _chain_id(r), "sender_pubkey": r.take(32), "nonce": r.uint(8),
            "fee": r.uint(8), "expires_at_height": r.uint(8), "memo": r.bytes_(MEMO_CEILING)}
    tag = r.uint(1)
    if tag == 0:
        body["payload"] = {"type": "transfer", "to": r.take(32), "amount": r.uint(8)}
    elif tag == 1:
        body["payload"] = {"type": "burn", "amount": r.uint(8)}
    elif tag == 2:
        att = _attestation(r)
        body["payload"] = {"type": "claim", "attestation": att, "issuer_signature": r.take(64)}
    else:
        raise DecodeError("invalid_enum_tag")
    sig = r.take(64)
    r.finish()
    return body, sig


# --------------------------------------------------------------------------- validation (§6.2, §6.3)
U64_MAX = 2**64 - 1

TESTNET_PARAMS = {
    "min_fee": 1000, "fee_burn_bps": 5000, "block_reward": 2_000_000, "reward_validators_bps": 6000,
    "reward_pool_bps": 3000, "reward_treasury_bps": 1000, "max_issuance": 10**15, "max_tx_bytes": 4096,
    "max_memo_bytes": 128, "max_tx_ttl_blocks": 1800, "epoch_length_blocks": 43200,
    "max_attestation_validity_s": 604800, "attestation_clock_skew_s": 300, "max_claims_per_subject_per_epoch": 20,
}
PARAM_FIELDS = [("min_fee", 8), ("fee_burn_bps", 2), ("block_reward", 8), ("reward_validators_bps", 2),
                ("reward_pool_bps", 2), ("reward_treasury_bps", 2), ("max_issuance", 8), ("max_tx_bytes", 4),
                ("max_memo_bytes", 4), ("max_tx_ttl_blocks", 8), ("epoch_length_blocks", 8),
                ("max_attestation_validity_s", 8), ("attestation_clock_skew_s", 8),
                ("max_claims_per_subject_per_epoch", 2)]


def encode_params(p):
    return b"".join(u(w, p[name]) for name, w in PARAM_FIELDS)


def stateless(tx_bytes, chain_id, params):
    """Returns (code, decoded-or-None)."""
    if len(tx_bytes) > params["max_tx_bytes"]:
        return "ERR_TOO_LARGE", None                                          # S1
    try:
        body, sig = decode_tx(tx_bytes)                                       # S2
    except DecodeError:
        return "ERR_DECODE", None
    dec = (body, sig)
    p = body["payload"]
    sender = account_id(body["sender_pubkey"])
    if body["version"] != 1:
        return "ERR_VERSION", dec                                             # S3
    if body["chain_id"] != chain_id:
        return "ERR_CHAIN_ID", dec                                            # S4
    if len(body["memo"]) > params["max_memo_bytes"]:
        return "ERR_MEMO", dec                                                # S5
    if body["fee"] < params["min_fee"]:
        return "ERR_FEE", dec                                                 # S6
    if body["expires_at_height"] < 1:
        return "ERR_EXPIRED", dec                                             # S7
    if p["type"] == "transfer" and (p["amount"] < 1 or p["to"] == sender):
        return "ERR_PAYLOAD", dec                                             # S8
    if p["type"] == "burn" and p["amount"] < 1:
        return "ERR_PAYLOAD", dec                                             # S9
    if p["type"] == "claim":                                                  # S10
        a = p["attestation"]
        ok = (a["version"] == 1 and a["chain_id"] == body["chain_id"] and a["subject"] == sender
              and a["issued_at_s"] < a["expires_at_s"]
              and a["expires_at_s"] - a["issued_at_s"] <= params["max_attestation_validity_s"])
        if not ok:
            return "ERR_ATTESTATION", dec
    if not ed.verify(body["sender_pubkey"], TX_PREFIX + encode_body(body), sig):
        return "ERR_SIGNATURE", dec                                           # S11
    return "OK", dec


def stateful(body, height, accounts, params):
    """accounts: dict account_id -> {"balance", "nonce"}. M1.2 stage: claims are ERR_UNSUPPORTED (§15)."""
    exp = body["expires_at_height"]
    if height > exp:
        return "ERR_EXPIRED"                                                  # T1
    if exp - height > params["max_tx_ttl_blocks"]:
        return "ERR_TTL_TOO_LONG"                                             # T2
    acct = accounts.get(account_id(body["sender_pubkey"]), {"balance": 0, "nonce": 0})
    if body["nonce"] != acct["nonce"]:
        return "ERR_NONCE"                                                    # T3
    if body["nonce"] == U64_MAX:
        return "ERR_OVERFLOW"                                                 # T4
    p = body["payload"]
    if p["type"] == "claim":
        return "ERR_UNSUPPORTED"                                              # T7 until M3
    total = p["amount"] + body["fee"]
    if total > U64_MAX:
        return "ERR_OVERFLOW"                                                 # T5
    if acct["balance"] < total:
        return "ERR_INSUFFICIENT_FUNDS"                                       # T6
    return "OK"


def validate(tx_bytes, height, chain_id, accounts, params):
    code, dec = stateless(tx_bytes, chain_id, params)
    if code != "OK":
        return code
    return stateful(dec[0], height, accounts, params)
