#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Remote queries of `yap network` (M2.6c) against a mock RPC: stable JSON, exact amounts, and the
difference between an absent account (exit 0, exists=false), an unknown transaction or block (exit 7)
and an RPC failure (exit 6); answers for another address or chain are refused.

    test_net_queries.py <yap>
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

FAILS = []
CHAIN = "yapchain-localnet-1"
ADDR = bech32m_ref.encode_address("tyap", bytes(range(32)))
TXB = bytes(range(10))
TXH = hashlib.sha256(TXB).hexdigest().upper()


def check(cond, msg):
    if not cond:
        FAILS.append(msg)
        print(f"FAIL: {msg}", file=sys.stderr)


def main(yap):
    mode = {"account": "present", "tx": "found", "block": "ok", "chain": CHAIN}

    def handler(method, p):
        if method == "status":
            return ("result", status_result(CHAIN, 77))
        if method == "abci_query":
            path = p["path"]
            if path == "/chain":
                return ("result", query_result(chain_answer(CHAIN, 77), 77))
            if path.startswith("/account/"):
                a = path[len("/account/"):]
                if mode["account"] == "rpc-error":
                    return ("error", -32603, "Internal error", "app unavailable")
                if mode["account"] == "garbage":
                    return ("raw", 200, b"{not json")
                if mode["account"] == "other":
                    a = "tyap1other"
                present = mode["account"] == "present"
                return ("result", query_result({"address": a, "account_id": "00" * 32, "exists": present,
                                                "balance": "18446744073709551615" if present else "0",
                                                "nonce": "7" if present else "0"}, 77))
        if method == "tx":
            if mode["tx"] == "missing":
                return ("error", -32603, "Internal error", f"tx ({TXH}) not found")
            if mode["tx"] == "noindex":
                return ("error", -32603, "Internal error", "transaction indexing is disabled")
            code = 0 if mode["tx"] == "found" else 11
            return ("result", {"hash": TXH, "height": "70", "index": 0, "tx_result": {"code": code, "log": ""},
                               "tx": base64.b64encode(TXB).decode()})
        if method == "block":
            if mode["block"] == "missing":
                return ("error", -32603, "Internal error",
                        "height 999 must be less than or equal to the current blockchain height 77")
            return ("result", {"block_id": {"hash": "CC" * 32},
                               "block": {"header": {"chain_id": mode["chain"], "height": "5", "time": "t",
                                                    "app_hash": "DD" * 32, "proposer_address": "EE" * 20},
                                         "data": {"txs": [base64.b64encode(TXB).decode()]},
                                         "last_commit": {"signatures": [{}, {}, {}]}}})
        return ("error", -32601, "Method not found", "")

    with tempfile.TemporaryDirectory() as home, MockRpc(handler) as m:
        def run(*args):
            return subprocess.run([yap, "--home", home, "network", "--rpc", m.url, "--chain-id", CHAIN, *args],
                                  capture_output=True, text=True, timeout=60)

        r = run("query", "account", ADDR, "--json")
        j = json.loads(r.stdout)
        check(r.returncode == 0, f"account rc {r.returncode} {r.stderr}")
        check(list(j) == ["address", "account_id", "exists", "balance_yip", "balance", "nonce", "height", "state"],
              f"stable keys {list(j)}")
        check(j["balance_yip"] == "18446744073709551615" and j["nonce"] == "7" and j["height"] == "77",
              f"u64 exact as strings {j}")
        check("18446744073709.551615 YAP" in j["balance"], f"exact amount formatting {j['balance']}")
        mode["account"] = "absent"
        r = run("query", "balance", ADDR, "--json")
        j = json.loads(r.stdout)
        check(r.returncode == 0 and j["exists"] is False and j["balance_yip"] == "0", f"absent account {r.stdout}")
        mode["account"] = "rpc-error"
        r = run("query", "balance", ADDR)
        check(r.returncode == 6, f"rpc error -> 6, got {r.returncode}")
        mode["account"] = "garbage"
        r = run("query", "balance", ADDR)
        check(r.returncode == 6, f"malformed -> 6, got {r.returncode}")
        mode["account"] = "other"
        r = run("query", "balance", ADDR)
        check(r.returncode == 6 and "another address" in r.stderr, f"answer for another address: {r.stderr}")
        mode["account"] = "present"

        r = run("query", "tx", TXH.lower(), "--json")
        j = json.loads(r.stdout)
        check(r.returncode == 0 and j["status"] == "finalized" and j["height"] == "70", f"tx found {r.stdout}")
        mode["tx"] = "failed"
        r = run("tx", "0x" + TXH)
        check(r.returncode == 9 and "finalized_failed" in r.stdout, f"failed execution -> 9: {r.returncode}")
        mode["tx"] = "missing"
        r = run("tx", TXH)
        check(r.returncode == 7 and "not in this node's transaction index" in r.stderr and "not proof" in r.stderr, f"missing tx -> 7 {r.returncode}")
        mode["tx"] = "noindex"
        r = run("tx", TXH)
        check(r.returncode == 6 and "indexing is disabled" in r.stderr, f"no indexer -> 6 {r.returncode}")
        r = run("tx", "abc")
        check(r.returncode == 2, f"bad hash -> 2, got {r.returncode}")

        r = run("query", "block", "5", "--json")
        j = json.loads(r.stdout)
        check(r.returncode == 0 and j["tx_hashes"] == [TXH] and j["last_commit_signatures"] == "3", f"block {r.stdout}")
        mode["block"] = "missing"
        r = run("query", "block", "999")
        check(r.returncode == 7, f"missing block -> 7, got {r.returncode}")
        mode["block"], mode["chain"] = "ok", "other-chain"
        r = run("query", "block", "5")
        check(r.returncode == 10, f"block of another chain -> 10, got {r.returncode}")

        r = run("query", "chain", "--json")
        j = json.loads(r.stdout)
        check(r.returncode == 0 and j["total_supply_yip"] == "5" and "no historical" in j["state"], f"chain {j}")
        # Read-only commands never broadcast.
        check(m.count("broadcast_tx_sync") == 0, "a query broadcast something")
    if FAILS:
        print(f"{len(FAILS)} failure(s)", file=sys.stderr)
        return 1
    print("remote queries: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(os.path.abspath(sys.argv[1])))
