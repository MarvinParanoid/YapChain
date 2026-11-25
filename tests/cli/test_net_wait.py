#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Finalization wait of `yap network wait` / `--wait` (M2.6e, M2.6.1) against a mock RPC.

    test_net_wait.py <yap>

Invariant: the CLI never claims that a transaction was not executed unless the data proves it — and
the RPC's transaction index cannot prove absence (it is written asynchronously, can be incomplete or
differ between nodes). Past expires_at_height the CLI reports two facts: the inclusion window is
closed, and the execution is unknown (exit 8). Scripted index/status sequences reproduce the races.
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
    raw = signed()
    h = hashlib.sha256(raw).hexdigest().upper()
    st = {}

    def reset(tx=("missing",), heights=None, catching_up=False, code=0, identity_height=100):
        """tx: answers of successive tx() calls ("missing" | "found" | "close" | "noindex"), the last
        one repeats. heights: answers of successive status() calls after the identity check (None =
        grows by one per call from 100)."""
        st.clear()
        st.update({"tx": list(tx), "tx_calls": 0, "heights": list(heights) if heights else None, "status_calls": 0,
                   "height": identity_height, "catching_up": catching_up, "code": code})

    def next_of(seq, i):
        return seq[min(i, len(seq) - 1)]

    def handler(method, p):
        if method == "status":
            st["status_calls"] += 1
            if st["status_calls"] > 1:  # the first call is the identity check
                if st["heights"]:
                    st["height"] = next_of(st["heights"], st["status_calls"] - 2)
                else:
                    st["height"] += 1
                if st["height"] == "close":
                    return ("close",)
            return ("result", status_result(CHAIN, st["height"], st["catching_up"]))
        if method == "abci_query" and p["path"] == "/chain":
            return ("result", query_result(chain_answer(CHAIN, 100), 100))
        if method == "broadcast_tx_sync":
            return ("result", {"code": 0, "log": "", "hash": h})
        if method == "tx":
            a = next_of(st["tx"], st["tx_calls"])
            st["tx_calls"] += 1
            if a == "close":
                return ("close",)
            if a == "noindex":
                return ("error", -32603, "Internal error", "transaction indexing is disabled")
            if a == "found":
                return ("result", {"hash": h, "height": "480", "index": 0,
                                   "tx_result": {"code": st["code"], "log": ""}, "tx": base64.b64encode(raw).decode()})
            return ("error", -32603, "Internal error", f"tx ({h}) not found")
        return ("error", -32601, "Method not found", "")

    with tempfile.TemporaryDirectory() as home, MockRpc(handler) as m:
        f = os.path.join(home, "t.hex")
        open(f, "w").write(raw.hex())

        def run(*args):
            t0 = time.time()
            r = subprocess.run([yap, "--home", home, "network", "--rpc", m.url, "--chain-id", CHAIN, *args],
                               capture_output=True, text=True, timeout=120)
            try:
                j = json.loads(r.stdout) if r.stdout.strip() else {}
            except ValueError:
                j = {}
            return r, j, time.time() - t0

        def never_claims_not_executed(r, j, what):
            text = (r.stdout + r.stderr).lower()
            check(j.get("status") != "expired" and "not executed" not in text.replace("not proof that it was not executed", ""),
                  f"{what}: false claim of non-execution: {r.stdout} {r.stderr}")

        # 1. executed between tx() and status(): the re-check right after status() finds it
        reset(tx=("missing", "found"), heights=[600])
        r, j, _ = run("wait", h, "--expires", "500", "--poll-ms", "100", "--json")
        check(r.returncode == 0 and j.get("status") == "finalized", f"1 race: {r.returncode} {r.stdout}")
        check(st["tx_calls"] == 2, f"1 race: re-checked once, got {st['tx_calls']} tx calls")

        # 2. the indexer lags the block store: found during the grace re-checks
        reset(tx=("missing", "missing", "missing", "found"), heights=[600])
        r, j, _ = run("wait", h, "--expires", "500", "--poll-ms", "200", "--json")
        check(r.returncode == 0 and j.get("status") == "finalized", f"2 lagging index: {r.returncode} {r.stdout}")

        # 3. executed, but absent from an incomplete index; 4. never executed at all — observably the same
        for label in ("3 incomplete index", "4 never executed"):
            reset(tx=("missing",), heights=[600])
            r, j, dt = run("wait", h, "--expires", "500", "--poll-ms", "200", "--timeout", "60", "--json")
            check(r.returncode == 8 and j.get("status") == "unknown" and j.get("execution") == "unknown"
                  and j.get("inclusion_window") == "closed", f"{label}: {r.returncode} {r.stdout}")
            check(j.get("node_height") == "600" and j.get("expires_at_height") == "500", f"{label}: facts {j}")
            check("NOT proof" in j.get("note", ""), f"{label}: note {j}")
            check(dt < 15, f"{label}: stops after the grace period, not the full timeout ({dt:.1f}s)")
            never_claims_not_executed(r, j, label)

        # 5. catching_up: heights still only grow, so a closed window is a fact; execution stays unknown
        reset(tx=("missing",), heights=[600], catching_up=True)
        r, j, _ = run("wait", h, "--expires", "500", "--poll-ms", "200", "--json")
        check(r.returncode == 8 and j.get("inclusion_window") == "closed" and j.get("execution") == "unknown",
              f"5 catching_up: {r.returncode} {r.stdout}")
        never_claims_not_executed(r, j, "5 catching_up")

        # 6. RPC unavailable during the re-check
        reset(tx=("missing", "close"), heights=[600, "close"])
        r, j, _ = run("wait", h, "--expires", "500", "--poll-ms", "200", "--json")
        check(r.returncode == 8 and j.get("status") == "unknown" and "last_rpc_error" in j, f"6 rpc down: {r.stdout}")
        never_claims_not_executed(r, j, "6 rpc down")

        # 7. appears in the index after a first "not found" (before the expiry)
        reset(tx=("missing", "missing", "found"))
        r, j, _ = run("wait", h, "--poll-ms", "100", "--json")
        check(r.returncode == 0 and j.get("status") == "finalized" and st["tx_calls"] == 3, f"7 late index: {r.stdout}")

        # 8. timeout with stalled consensus and no expiry known: unknown, never "rejected"
        reset(tx=("missing",), heights=[100])
        r, j, dt = run("wait", h, "--timeout", "12", "--poll-ms", "500", "--json")
        check(r.returncode == 8 and j.get("status") == "unknown" and "NOT mean rejected" in j.get("note", ""),
              f"8 timeout: {r.stdout}")
        check("consensus may be stalled" in r.stderr and 11 <= dt < 20, f"8 timeout: stall note / bounded ({dt:.1f}s)")
        check(10 <= st["tx_calls"] <= 30, f"8 poll interval respected: {st['tx_calls']}")
        never_claims_not_executed(r, j, "8 timeout")

        # 9. finalized with an execution error is not "unknown"
        reset(tx=("found",), code=11)
        r, j, _ = run("wait", h, "--poll-ms", "100", "--json")
        check(r.returncode == 9 and j.get("status") == "finalized_failed", f"9 failed: {r.stdout}")
        reset(tx=("missing", "found"), heights=[600], code=11)
        r, j, _ = run("wait", h, "--expires", "500", "--poll-ms", "100", "--json")
        check(r.returncode == 9 and j.get("status") == "finalized_failed", f"9 failed after the window: {r.stdout}")

        # the indexer is disabled: an error (6), not a verdict
        reset(tx=("noindex",))
        r, j, _ = run("wait", h, "--poll-ms", "100")
        check(r.returncode == 6 and "kv" in r.stderr, f"no indexer {r.returncode} {r.stderr}")

        # send --wait: one broadcast, then the expiry from the transaction itself drives the same logic
        reset(tx=("missing",), heights=[600])
        n = m.count("broadcast_tx_sync")
        r, j, _ = run("send", f, "--wait", "--poll-ms", "200", "--json")
        check(r.returncode == 8 and j.get("inclusion_window") == "closed" and m.count("broadcast_tx_sync") == n + 1,
              f"send --wait past expiry: {r.returncode} {r.stdout}")
        never_claims_not_executed(r, j, "send --wait")
        reset(tx=("missing", "found"))
        r, j, _ = run("send", f, "--wait", "--poll-ms", "100", "--json")
        check(r.returncode == 0 and j.get("status") == "finalized", f"send --wait: {r.stdout}")
        n = m.count("broadcast_tx_sync")
        reset(tx=("found",))
        r, _, _ = run("tx", h)
        check(r.returncode == 0 and m.count("broadcast_tx_sync") == n, "re-check by hash without re-sending")
        for bad in (["--timeout", "0"], ["--timeout", "999999"], ["--poll-ms", "5"]):
            r, _, _ = run("wait", h, *bad)
            check(r.returncode == 1, f"{bad} accepted")
    if FAILS:
        print(f"{len(FAILS)} failure(s)", file=sys.stderr)
        return 1
    print("finalization wait: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(os.path.abspath(sys.argv[1])))
