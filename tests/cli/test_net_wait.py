#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Finalization wait of `yap network wait` / `--wait` (M2.6e) against a mock RPC.

    test_net_wait.py <yap>

Bounded timeout and polling; a timeout is reported as unknown (8), never as rejected; stalled
consensus is noted; "expired" (2) only when the node is past expires_at_height and not catching up;
failed execution (9); disabled indexer (6); an unreachable node during the wait ends as unknown (8);
the result can be re-checked by hash without re-sending.
"""
import base64
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "refgen"))
from mock_rpc import MockRpc, chain_answer, query_result, status_result  # noqa: E402
import ed25519_ref  # noqa: E402
import tx_ref  # noqa: E402

FAILS = []
CHAIN = "yapchain-localnet-1"


def check(cond, msg):
    if not cond:
        FAILS.append(msg)
        print(f"FAIL: {msg}", file=sys.stderr)


def signed(expires=500):
    seed = bytes([9]) * 32
    body = {"version": 1, "chain_id": CHAIN, "sender_pubkey": ed25519_ref.public_key(seed), "nonce": 0, "fee": 1000,
            "expires_at_height": expires, "memo": b"", "payload": {"type": "burn", "amount": 5}}
    return tx_ref.sign_tx(body, seed)


def main(yap):
    st = {"height": 100, "advance": True, "found_after": 2, "code": 0, "tx_calls": 0, "mode": "normal",
          "catching_up": False}
    raw = signed()
    h = hashlib.sha256(raw).hexdigest().upper()

    def handler(method, p):
        # "down": the node becomes unreachable after the first transaction poll of the wait
        if st["mode"] == "down" and st["tx_calls"] >= 1 and method in ("tx", "status"):
            return ("close",)
        if method == "status":
            if st["advance"]:
                st["height"] += 1
            return ("result", status_result(CHAIN, st["height"], st["catching_up"]))
        if method == "abci_query" and p["path"] == "/chain":
            return ("result", query_result(chain_answer(CHAIN, st["height"]), st["height"]))
        if method == "broadcast_tx_sync":
            return ("result", {"code": 0, "log": "", "hash": h})
        if method == "tx":
            st["tx_calls"] += 1
            if st["mode"] == "noindex":
                return ("error", -32603, "Internal error", "transaction indexing is disabled")
            if st["found_after"] is not None and st["tx_calls"] > st["found_after"]:
                return ("result", {"hash": h, "height": str(st["height"]), "index": 0,
                                   "tx_result": {"code": st["code"], "log": ""}, "tx": base64.b64encode(raw).decode()})
            return ("error", -32603, "Internal error", f"tx ({h}) not found")
        return ("error", -32601, "Method not found", "")

    def reset(**kw):
        st.update({"height": 100, "advance": True, "found_after": 2, "code": 0, "tx_calls": 0, "mode": "normal",
                   "catching_up": False})
        st.update(kw)

    with tempfile.TemporaryDirectory() as home, MockRpc(handler) as m:
        f = os.path.join(home, "t.hex")
        open(f, "w").write(raw.hex())

        def run(*args):
            t0 = time.time()
            r = subprocess.run([yap, "--home", home, "network", "--rpc", m.url, "--chain-id", CHAIN, *args],
                               capture_output=True, text=True, timeout=120)
            return r, time.time() - t0

        reset()
        r, _ = run("wait", h, "--poll-ms", "100", "--json")
        j = json.loads(r.stdout)
        check(r.returncode == 0 and j["status"] == "finalized" and j["result"] == "OK", f"finalized {r.stdout}")
        check(st["tx_calls"] == 3, f"polled {st['tx_calls']} times")
        reset(code=11)
        r, _ = run("wait", h, "--poll-ms", "100", "--json")
        check(r.returncode == 9 and json.loads(r.stdout)["status"] == "finalized_failed", f"failed -> 9 {r.stdout}")
        # consensus stalled: height does not move; timeout -> unknown, never "rejected"
        reset(found_after=None, advance=False)
        r, dt = run("wait", h, "--timeout", "12", "--poll-ms", "500", "--json")
        j = json.loads(r.stdout)
        check(r.returncode == 8 and j["status"] == "unknown" and "NOT mean rejected" in j["note"], f"timeout {r.stdout}")
        check("consensus may be stalled" in r.stderr, "stall note")
        check(11 <= dt < 20, f"bounded wait {dt:.1f}s")
        check(10 <= st["tx_calls"] <= 30, f"poll interval respected: {st['tx_calls']} polls")
        # past the expiry height, not catching up -> expired (it can never be included)
        reset(found_after=None, height=600)
        r, _ = run("wait", h, "--expires", "500", "--poll-ms", "100", "--json")
        j = json.loads(r.stdout)
        check(r.returncode == 2 and j["status"] == "expired", f"expired {r.stdout}")
        reset(found_after=None, height=600, catching_up=True)
        r, _ = run("wait", h, "--expires", "500", "--timeout", "2", "--poll-ms", "200")
        check(r.returncode == 8, f"no expiry verdict while the node catches up: {r.returncode}")
        # indexer disabled
        reset(mode="noindex")
        r, _ = run("wait", h, "--poll-ms", "100")
        check(r.returncode == 6 and "kv" in r.stderr, f"no indexer {r.returncode} {r.stderr}")
        # node unreachable during the wait: keeps trying until the deadline, then unknown
        reset(mode="down", found_after=None)
        r, dt = run("wait", h, "--timeout", "3", "--poll-ms", "200", "--json")
        j = json.loads(r.stdout)
        check(r.returncode == 8 and j["status"] == "unknown" and "last_rpc_error" in j, f"down {r.stdout}")
        # send --wait: one broadcast, then finalization; a later check by hash does not re-send
        reset()
        n = m.count("broadcast_tx_sync")
        r, _ = run("send", f, "--wait", "--poll-ms", "100", "--json")
        check(r.returncode == 0 and json.loads(r.stdout)["status"] == "finalized", f"send --wait {r.stdout}")
        r, _ = run("tx", h)
        check(r.returncode == 0 and m.count("broadcast_tx_sync") == n + 1, "re-check by hash without re-sending")
        for bad in (["--timeout", "0"], ["--timeout", "999999"], ["--poll-ms", "5"]):
            r, _ = run("wait", h, *bad)
            check(r.returncode == 1, f"{bad} accepted")
    if FAILS:
        print(f"{len(FAILS)} failure(s)", file=sys.stderr)
        return 1
    print("finalization wait: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(os.path.abspath(sys.argv[1])))
