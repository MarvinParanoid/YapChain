#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Broadcast semantics of `yap network send|transfer|burn` (M2.6d) against a mock RPC.

    test_net_send.py <yap>

CheckTx rejection (2), mempool acceptance (0, "not final"), a node that refuses (2), already known
bytes, an unknown outcome after the request left (8) — never re-sent —, a refused connection (6, not
sent); local refusal of another chain's transaction (10) and of an invalid signature (2) without any
broadcast; confirmation required off a terminal; the signed transaction decoded by the independent
reference carries the expected chain id, the node's nonce/fee/height + ttl, or the given overrides.
"""
import base64
import hashlib
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "refgen"))
from mock_rpc import MockRpc, chain_answer, query_result, status_result  # noqa: E402
import bech32m_ref  # noqa: E402
import ed25519_ref  # noqa: E402
import tx_ref  # noqa: E402

FAILS = []
CHAIN = "yapchain-localnet-1"
TO = bech32m_ref.encode_address("tyap", bytes(range(32)))


def check(cond, msg):
    if not cond:
        FAILS.append(msg)
        print(f"FAIL: {msg}", file=sys.stderr)


def signed(chain=CHAIN, nonce=0, corrupt=False):
    seed = bytes([7]) * 32
    body = {"version": 1, "chain_id": chain, "sender_pubkey": ed25519_ref.public_key(seed), "nonce": nonce, "fee": 1000,
            "expires_at_height": 500, "memo": b"", "payload": {"type": "burn", "amount": 5}}
    raw = bytearray(tx_ref.sign_tx(body, seed))
    if corrupt:
        raw[-1] ^= 1
    return bytes(raw)


def main(yap):
    mode = {"broadcast": "ok", "tx": "missing"}
    sent = []

    def handler(method, p):
        if method == "status":
            return ("result", status_result(CHAIN, 77))
        if method == "abci_query":
            if p["path"] == "/chain":
                return ("result", query_result(chain_answer(CHAIN, 77), 77))
            if p["path"] == "/params":
                return ("result", query_result({"min_fee": "1500", "max_tx_ttl_blocks": "1800"}, 77))
            if p["path"].startswith("/account/"):
                a = p["path"][len("/account/"):]
                return ("result", query_result({"address": a, "account_id": "00" * 32, "exists": True,
                                                "balance": "1000000000", "nonce": "3"}, 75))
        if method == "broadcast_tx_sync":
            raw = base64.b64decode(p["tx"])
            sent.append(raw)
            h = hashlib.sha256(raw).hexdigest().upper()
            m = mode["broadcast"]
            if m == "ok":
                return ("result", {"code": 0, "log": "", "hash": h})
            if m == "checktx":
                return ("result", {"code": 10, "log": "ERR_NONCE", "hash": h})
            if m == "full":
                return ("error", -32603, "Internal error", "mempool is full: number of txs 5000 (max: 5000)")
            if m == "cache":
                return ("error", -32603, "Internal error", "tx already exists in cache")
            if m == "close":
                return ("close",)
        if method == "tx":
            hx = base64.b64decode(p["hash"]).hex().upper()
            if mode["tx"] == "found" and sent:
                return ("result", {"hash": hx, "height": "60", "index": 0, "tx_result": {"code": 0, "log": ""},
                                   "tx": base64.b64encode(sent[-1]).decode()})
            return ("error", -32603, "Internal error", f"tx ({hx}) not found")
        return ("error", -32601, "Method not found", "")

    with tempfile.TemporaryDirectory() as home, MockRpc(handler) as m:
        pw = os.path.join(home, "pw")
        fd = os.open(pw, os.O_WRONLY | os.O_CREAT, 0o600)
        os.write(fd, b"correct horse battery staple\n")
        os.close(fd)

        def run(*args, yes=True, chain=CHAIN):
            cmd = [yap, "--home", home, "--password-file", pw, "network", "--rpc", m.url, "--chain-id", chain, *args]
            return subprocess.run(cmd, capture_output=True, text=True, timeout=60, stdin=subprocess.DEVNULL)

        r = subprocess.run([yap, "--home", home, "--password-file", pw, "keys", "create", "alice"],
                           capture_output=True, text=True)
        check(r.returncode == 0, f"keys create: {r.stderr}")
        key_file = open(os.path.join(home, "keys", "alice.json")).read()

        # transfer through the keystore: nonce, fee and expiry from the node
        r = run("transfer", "--from", "alice", "--to", TO, "--amount", "2.5YAP", "--yes", "--json")
        j = json.loads(r.stdout)
        check(r.returncode == 0 and j["status"] == "accepted_mempool" and "NOT final" in j["note"],
              f"accepted: {r.returncode} {r.stdout} {r.stderr}")
        body, _ = tx_ref.decode_tx(sent[-1])
        check(body["chain_id"] == CHAIN and body["nonce"] == 3 and body["fee"] == 1500 and
              body["expires_at_height"] == 177 and body["payload"]["amount"] == 2_500_000,
              f"signed body {body}")
        check(j["hash"] == hashlib.sha256(sent[-1]).hexdigest().upper(), "hash computed locally")
        # overrides
        r = run("burn", "--from", "alice", "--amount", "7", "--nonce", "9", "--expires", "90", "--fee", "2000",
                "--yes", "--out", os.path.join(home, "b.hex"))
        body, _ = tx_ref.decode_tx(sent[-1])
        check(r.returncode == 0 and body["nonce"] == 9 and body["expires_at_height"] == 90 and body["fee"] == 2000,
              f"overrides {body}")
        check(open(os.path.join(home, "b.hex")).read().strip() == sent[-1].hex(), "--out writes the signed tx")
        # confirmation is required off a terminal
        n = len(sent)
        r = run("transfer", "--from", "alice", "--to", TO, "--amount", "1")
        check(r.returncode == 1 and "--yes" in r.stderr and len(sent) == n, f"no --yes: {r.returncode} {r.stderr}")
        # no expected chain id: nothing is signed
        r = subprocess.run([yap, "--home", home, "--password-file", pw, "network", "--rpc", m.url, "transfer",
                            "--from", "alice", "--to", TO, "--amount", "1", "--yes"], capture_output=True, text=True,
                           stdin=subprocess.DEVNULL, env={k: v for k, v in os.environ.items() if k != "YAP_CHAIN_ID"})
        check(r.returncode == 1 and "no expected chain id" in r.stderr and len(sent) == n, "chain id required to sign")

        # send of a prepared file
        good = os.path.join(home, "good.hex")
        open(good, "w").write(signed().hex())
        mode["broadcast"] = "checktx"
        r = run("send", good, "--json")
        j = json.loads(r.stdout)
        check(r.returncode == 2 and j["status"] == "rejected" and j["result"] == "ERR_NONCE", f"checktx {r.stdout}")
        # ERR_NONCE may mean that this very transaction was already executed: never "not executed"
        check(j.get("execution", "").startswith("unknown"), f"ERR_NONCE without index entry: {j}")
        mode["tx"] = "found"
        r = run("send", good, "--json")
        j = json.loads(r.stdout)
        check(r.returncode == 0 and j["status"] == "finalized" and "already executed" in j.get("broadcast", ""),
              f"ERR_NONCE of an executed transaction: {r.returncode} {r.stdout}")
        mode["tx"] = "missing"
        mode["broadcast"] = "full"
        r = run("send", good)
        check(r.returncode == 2 and "did not accept" in r.stderr, f"mempool full {r.returncode} {r.stderr}")
        mode["broadcast"] = "cache"
        r = run("send", good, "--json")
        check(r.returncode == 8 and json.loads(r.stdout)["status"] == "already_known", f"cache {r.stdout}")
        mode["broadcast"] = "close"
        n = m.count("broadcast_tx_sync")
        r = run("send", good, "--json")
        j = json.loads(r.stdout)
        check(r.returncode == 8 and j["status"] == "unknown" and "not sent again" in r.stderr, f"unknown {r.stdout}")
        check(m.count("broadcast_tx_sync") == n + 1, "the broadcast was repeated")
        r = subprocess.run([yap, "--home", home, "network", "--rpc", "http://127.0.0.1:1", "--chain-id", CHAIN, "send",
                            good], capture_output=True, text=True)
        check(r.returncode == 6 and "not sent" in r.stderr, f"refused connection {r.returncode} {r.stderr}")
        # refused locally, nothing sent
        mode["broadcast"] = "ok"
        n = m.count("broadcast_tx_sync")
        other = os.path.join(home, "other.hex")
        open(other, "w").write(signed(chain="other-chain").hex())
        r = run("send", other)
        check(r.returncode == 10 and "signed for chain 'other-chain'" in r.stderr, f"other chain {r.stderr}")
        bad = os.path.join(home, "bad.hex")
        open(bad, "w").write(signed(corrupt=True).hex())
        r = run("send", bad)
        check(r.returncode == 2 and "signature is invalid" in r.stderr, f"bad signature {r.stderr}")
        check(m.count("broadcast_tx_sync") == n, "a locally refused transaction was sent")
        r = run("send", bad, "--skip-local-checks")
        check(r.returncode == 0 and m.count("broadcast_tx_sync") == n + 1, "--skip-local-checks sends")
        r = run("send", other, "--skip-local-checks")
        check(r.returncode == 10, "the chain id check is never skipped")

        # nothing secret ever left: requests carry public data only
        password = "correct horse battery staple"
        enc = json.loads(key_file)
        blob = json.dumps(m.requests)
        check(password not in blob, "the password reached the RPC")
        for v in enc.values() if isinstance(enc, dict) else []:
            if isinstance(v, str) and len(v) >= 32 and v not in (enc.get("public_key"), enc.get("address")):
                check(v not in blob, "keystore material reached the RPC")
    if FAILS:
        print(f"{len(FAILS)} failure(s)", file=sys.stderr)
        return 1
    print("broadcast semantics: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(os.path.abspath(sys.argv[1])))
