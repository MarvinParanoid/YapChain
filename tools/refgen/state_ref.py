# SPDX-License-Identifier: Apache-2.0
"""Independent reference for state encodings (protocol.md §7) and genesis (§11, ADR-0016, ADR-0021)."""
import hashlib
import json
import re

import bech32m_ref as b32
import ed25519_ref as ed
import merkle_ref
import tx_ref

U64_MAX = 2**64 - 1
u = tx_ref.u
enc_bytes = tx_ref.enc_bytes


def consensus_address(pub):
    return hashlib.sha256(pub).digest()[:20]


# ------------------------------------------------------------------ keys and values
def key_chain_state():
    return b"\x00"


def key_params():
    return b"\x02"


def key_account(aid):
    return b"\x01" + aid


def key_validator(addr):
    return b"\x03" + addr


def key_issuer(i):
    return b"\x04" + u(4, i)


def key_achievement(i, a):
    return b"\x06" + u(4, i) + u(4, a)


def enc_chain_state(cs):
    return b"".join(u(8, cs[k]) for k in ("height", "last_time_ms", "total_supply", "total_minted", "total_burned"))


def enc_account(balance, nonce):
    return u(8, balance) + u(8, nonce)


def enc_validator(v):
    return enc_bytes(v["name"].encode()) + v["consensus_pubkey"] + u(8, v["power"]) + v["reward_account"]


def enc_issuer(i):
    out = u(4, i["issuer_id"]) + enc_bytes(i["name"].encode()) + u(1, i["status"]) + u(8, i["epoch_budget"])
    out += u(4, len(i["keys"]))
    for k in i["keys"]:
        out += u(1, k["key_id"]) + k["pubkey"] + u(1, k["status"])
    return out


def enc_achievement(a):
    return (u(4, a["issuer_id"]) + u(4, a["achievement_id"]) + u(8, a["reward"]) + u(1, a["policy"])
            + u(2, a["max_per_epoch"]) + u(1, a["status"]))


# ------------------------------------------------------------------ genesis
def encode_genesis(g):
    out = enc_bytes(g["chain_id"].encode()) + u(8, g["genesis_time_ms"]) + u(8, g["initial_height"])
    out += tx_ref.encode_params(g["params"])
    out += u(4, len(g["accounts"])) + b"".join(a["account_id"] + u(8, a["balance"]) for a in g["accounts"])
    out += u(4, len(g["validators"])) + b"".join(
        v["consensus_pubkey"] + u(8, v["power"]) + v["reward_account"] + enc_bytes(v["name"].encode())
        for v in g["validators"])
    out += u(4, len(g["issuers"])) + b"".join(enc_issuer(i) for i in g["issuers"])
    out += u(4, len(g["achievements"])) + b"".join(enc_achievement(a) for a in g["achievements"])
    return out


def genesis_hash(g):
    return tx_ref.tagged_hash("YAP/genesis/v1", encode_genesis(g))


def _name_ok(s):
    return 1 <= len(s) <= 32 and all(0x20 <= ord(c) <= 0x7E for c in s)


def params_error(p):
    if p["min_fee"] < 1 or p["fee_burn_bps"] > 10000:
        return "params"
    if p["reward_validators_bps"] + p["reward_pool_bps"] + p["reward_treasury_bps"] != 10000:
        return "params"
    if p["max_issuance"] < 1 or not 256 <= p["max_tx_bytes"] <= 65536 or p["max_memo_bytes"] > 1024:
        return "params"
    if min(p["max_tx_ttl_blocks"], p["epoch_length_blocks"], p["max_attestation_validity_s"],
           p["max_claims_per_subject_per_epoch"]) < 1:
        return "params"
    return None


def validate_genesis(g):
    """protocol.md §11.2; returns an error string or None."""
    if not tx_ref.valid_chain_id(g["chain_id"]) or g["initial_height"] != 1:
        return "chain"
    if params_error(g["params"]):
        return "params"
    ids = [a["account_id"] for a in g["accounts"]]
    if ids != sorted(set(ids)) or any(a["balance"] < 1 for a in g["accounts"]):
        return "accounts"
    if sum(a["balance"] for a in g["accounts"]) > g["params"]["max_issuance"]:
        return "accounts"
    vs = g["validators"]
    if not 1 <= len(vs) <= 32:
        return "validators"
    addrs = [consensus_address(v["consensus_pubkey"]) for v in vs]
    if addrs != sorted(set(addrs)):
        return "validators"
    if any(not ed.is_valid_validator_key(v["consensus_pubkey"]) or v["power"] < 1 or not _name_ok(v["name"])
           for v in vs) or sum(v["power"] for v in vs) > 2**60:
        return "validators"
    iids = [i["issuer_id"] for i in g["issuers"]]
    if iids != sorted(set(iids)):
        return "issuers"
    for i in g["issuers"]:
        kids = [k["key_id"] for k in i["keys"]]
        if (not _name_ok(i["name"]) or i["status"] not in (1, 2) or not 1 <= len(kids) <= 8
                or kids != sorted(set(kids)) or any(k["status"] not in (1, 2) for k in i["keys"])):
            return "issuers"
    aids = [(a["issuer_id"], a["achievement_id"]) for a in g["achievements"]]
    if aids != sorted(set(aids)):
        return "achievements"
    for a in g["achievements"]:
        if a["issuer_id"] not in iids or a["reward"] < 1 or a["status"] not in (1, 2):
            return "achievements"
        if not ((a["policy"] == 1 and a["max_per_epoch"] == 0) or (a["policy"] == 2 and a["max_per_epoch"] >= 1)):
            return "achievements"
    return None


def initial_state(g):
    """§11.4 item 3: dict raw_key -> value bytes."""
    st = {key_params(): tx_ref.encode_params(g["params"])}
    for a in g["accounts"]:
        st[key_account(a["account_id"])] = enc_account(a["balance"], 0)
    for v in g["validators"]:
        st[key_validator(consensus_address(v["consensus_pubkey"]))] = enc_validator(v)
    for i in g["issuers"]:
        st[key_issuer(i["issuer_id"])] = enc_issuer(i)
    for a in g["achievements"]:
        st[key_achievement(a["issuer_id"], a["achievement_id"])] = enc_achievement(a)
    supply = sum(a["balance"] for a in g["accounts"])
    st[key_chain_state()] = enc_chain_state({"height": 0, "last_time_ms": g["genesis_time_ms"],
                                             "total_supply": supply, "total_minted": supply, "total_burned": 0})
    return st


# ------------------------------------------------------------------ JSON form (§11.3)
SMALL = {"fee_burn_bps", "reward_validators_bps", "reward_pool_bps", "reward_treasury_bps", "max_tx_bytes",
         "max_memo_bytes", "max_claims_per_subject_per_epoch"}


def to_json_obj(g, hrp="tyap"):
    p = {k: (v if k in SMALL else str(v)) for k, v in g["params"].items()}
    return {
        "chain_id": g["chain_id"], "genesis_time_ms": str(g["genesis_time_ms"]),
        "initial_height": str(g["initial_height"]), "params": p,
        "accounts": [{"address": b32.encode_address(hrp, a["account_id"]), "balance": str(a["balance"])}
                     for a in g["accounts"]],
        "validators": [{"consensus_pubkey": v["consensus_pubkey"].hex(), "power": str(v["power"]),
                        "reward_account": b32.encode_address(hrp, v["reward_account"]), "name": v["name"]}
                       for v in g["validators"]],
        "issuers": [{"issuer_id": i["issuer_id"], "name": i["name"], "status": i["status"],
                     "epoch_budget": str(i["epoch_budget"]),
                     "keys": [{"key_id": k["key_id"], "pubkey": k["pubkey"].hex(), "status": k["status"]}
                              for k in i["keys"]]} for i in g["issuers"]],
        "achievements": [{"issuer_id": a["issuer_id"], "achievement_id": a["achievement_id"],
                          "reward": str(a["reward"]), "policy": a["policy"], "max_per_epoch": a["max_per_epoch"],
                          "status": a["status"]} for a in g["achievements"]],
    }


class JsonSchemaError(Exception):
    pass


def _pairs(pairs):
    keys = [k for k, _ in pairs]
    if len(keys) != len(set(keys)):
        raise JsonSchemaError("duplicate key")
    return dict(pairs)


def _exact(obj, names):
    if not isinstance(obj, dict) or set(obj) != set(names):
        raise JsonSchemaError("fields")


def _u64(v):
    if not isinstance(v, str) or not re.fullmatch(r"0|[1-9][0-9]{0,19}", v) or int(v) > U64_MAX:
        raise JsonSchemaError("u64")
    return int(v)


def _small(v, bits):
    if isinstance(v, bool) or not isinstance(v, int) or v < 0 or v >= 2**bits:
        raise JsonSchemaError("small int")
    return v


def _str(v):
    if not isinstance(v, str):
        raise JsonSchemaError("string")
    return v


def _hex32(v):
    if not isinstance(v, str) or not re.fullmatch(r"[0-9a-f]{64}", v):
        raise JsonSchemaError("hex32")
    return bytes.fromhex(v)


def _addr(v, hrp):
    try:
        return b32.decode_address(_str(v).encode(), hrp)
    except ValueError:
        raise JsonSchemaError("address")


def parse_genesis_json(text):
    """Strict parser mirroring §11.3; returns the genesis dict or raises JsonSchemaError / ValueError."""
    def no_float(_):
        raise JsonSchemaError("float")

    root = json.loads(text, object_pairs_hook=_pairs, parse_float=no_float, parse_constant=no_float)
    _exact(root, ["chain_id", "genesis_time_ms", "initial_height", "params", "accounts", "validators", "issuers",
                  "achievements"])
    chain_id = _str(root["chain_id"])
    if not tx_ref.valid_chain_id(chain_id):
        raise JsonSchemaError("chain_id")
    hrp = "tyap"
    names = [n for n, _ in tx_ref.PARAM_FIELDS]
    _exact(root["params"], names)
    params = {n: (_small(root["params"][n], 8 * w) if n in SMALL else _u64(root["params"][n]))
              for n, w in tx_ref.PARAM_FIELDS}
    for k in ("accounts", "validators", "issuers", "achievements"):
        if not isinstance(root[k], list):
            raise JsonSchemaError("array")
    g = {"chain_id": chain_id, "genesis_time_ms": _u64(root["genesis_time_ms"]),
         "initial_height": _u64(root["initial_height"]), "params": params, "accounts": [], "validators": [],
         "issuers": [], "achievements": []}
    for a in root["accounts"]:
        _exact(a, ["address", "balance"])
        g["accounts"].append({"account_id": _addr(a["address"], hrp), "balance": _u64(a["balance"])})
    for v in root["validators"]:
        _exact(v, ["consensus_pubkey", "power", "reward_account", "name"])
        g["validators"].append({"consensus_pubkey": _hex32(v["consensus_pubkey"]), "power": _u64(v["power"]),
                                "reward_account": _addr(v["reward_account"], hrp), "name": _str(v["name"])})
    for i in root["issuers"]:
        _exact(i, ["issuer_id", "name", "status", "epoch_budget", "keys"])
        if not isinstance(i["keys"], list):
            raise JsonSchemaError("array")
        keys = []
        for k in i["keys"]:
            _exact(k, ["key_id", "pubkey", "status"])
            keys.append({"key_id": _small(k["key_id"], 8), "pubkey": _hex32(k["pubkey"]),
                         "status": _small(k["status"], 8)})
        g["issuers"].append({"issuer_id": _small(i["issuer_id"], 32), "name": _str(i["name"]),
                             "status": _small(i["status"], 8), "epoch_budget": _u64(i["epoch_budget"]),
                             "keys": keys})
    for a in root["achievements"]:
        _exact(a, ["issuer_id", "achievement_id", "reward", "policy", "max_per_epoch", "status"])
        g["achievements"].append({"issuer_id": _small(a["issuer_id"], 32),
                                  "achievement_id": _small(a["achievement_id"], 32), "reward": _u64(a["reward"]),
                                  "policy": _small(a["policy"], 8), "max_per_epoch": _small(a["max_per_epoch"], 16),
                                  "status": _small(a["status"], 8)})
    err = validate_genesis(g)
    if err:
        raise ValueError(err)
    return g
