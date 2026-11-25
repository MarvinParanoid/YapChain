#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Network configuration and chain identity of `yap network` (M2.6b), against a mock RPC.

    test_net_config.py <yap>

Precedence flag > environment > <home>/network.json > default; strict config file; the expected
chain id is never taken from the node; identity is checked against both the CometBFT network and the
application's /chain; local commands never read the network configuration.
"""
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mock_rpc import MockRpc, chain_answer, query_result, status_result  # noqa: E402

FAILS = []


def check(cond, msg):
    if not cond:
        FAILS.append(msg)
        print(f"FAIL: {msg}", file=sys.stderr)


def run(yap, home, *args, env=None):
    e = {k: v for k, v in os.environ.items() if k not in ("YAP_RPC", "YAP_CHAIN_ID")}
    e.update(env or {})
    return subprocess.run([yap, "--home", home, *args], capture_output=True, text=True, env=e, timeout=60)


def show(yap, home, *args, env=None):
    r = run(yap, home, "network", "config", "show", "--json", *args, env=env)
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)


def main(yap):
    node_chain = {"value": "yapchain-localnet-1", "app": "yapchain-localnet-1"}

    def handler(method, params):
        if method == "status":
            return ("result", status_result(node_chain["value"]))
        if method == "abci_query" and params["path"] == "/chain":
            return ("result", query_result(chain_answer(node_chain["app"])))
        return ("error", -32601, "Method not found", "")

    with tempfile.TemporaryDirectory() as home, MockRpc(handler) as m:
        # 1. defaults
        c = show(yap, home)
        check(c["rpc"] == "http://127.0.0.1:26657" and c["rpc_source"] == "default", f"default rpc {c}")
        check(c["chain_id"] == "(unset)" and c["chain_id_source"] == "unset", "chain id has no default")
        # 2. config file < env < flag
        r = run(yap, home, "network", "config", "set", "--rpc", m.url, "--chain-id", "yapchain-localnet-1",
                "--timeout-ms", "7000")
        check(r.returncode == 0, f"config set: {r.stderr}")
        stored = json.load(open(os.path.join(home, "network.json")))
        check(stored == {"rpc": m.url, "chain_id": "yapchain-localnet-1", "timeout_ms": 7000}, f"stored {stored}")
        c = show(yap, home)
        check(c["rpc_source"] == "config" and c["timeout_ms"] == "7000", f"config values {c}")
        c = show(yap, home, env={"YAP_RPC": "http://127.0.0.2:1", "YAP_CHAIN_ID": "env-chain"})
        check(c["rpc"] == "http://127.0.0.2:1" and c["rpc_source"] == "env" and c["chain_id"] == "env-chain",
              f"env over config {c}")
        c = show(yap, home, "--rpc", "http://127.0.0.3:2", "--chain-id", "flag-chain",
                 env={"YAP_RPC": "http://127.0.0.2:1", "YAP_CHAIN_ID": "env-chain"})
        check(c["rpc_source"] == "flag" and c["chain_id"] == "flag-chain", f"flag over env {c}")
        # 3. invalid values are refused, the file is strict
        for args in (["--rpc", "ftp://x"], ["--rpc", "http://u:p@h:1"], ["--chain-id", "Bad_Chain"],
                     ["--timeout-ms", "5"], ["--max-response-bytes", "10"], ["--ca-file", "/nonexistent"]):
            r = run(yap, home, "network", "config", "show", *args)
            check(r.returncode == 1, f"{args} accepted: {r.returncode}")
        with open(os.path.join(home, "network.json"), "w") as f:
            json.dump({"rpc": m.url, "chain_id": "yapchain-localnet-1", "colour": "blue"}, f)
        r = run(yap, home, "network", "config", "show")
        check(r.returncode == 1 and "unknown key 'colour'" in r.stderr, f"unknown key: {r.stderr}")
        with open(os.path.join(home, "network.json"), "w") as f:
            json.dump({"rpc": m.url, "chain_id": "yapchain-localnet-1"}, f)

        # 4. chain identity: both the node and the application must match the expected chain id
        r = run(yap, home, "network", "status", "--json")
        check(r.returncode == 0 and json.loads(r.stdout)["chain_id_verified"] is True, f"status ok: {r.stderr}")
        node_chain["value"] = "other-chain"
        r = run(yap, home, "network", "status")
        check(r.returncode == 10 and "is on chain 'other-chain'" in r.stderr, f"node mismatch: {r.returncode} {r.stderr}")
        node_chain["value"], node_chain["app"] = "yapchain-localnet-1", "other-app"
        r = run(yap, home, "network", "status")
        check(r.returncode == 10 and "application" in r.stderr, f"app mismatch: {r.returncode} {r.stderr}")
        node_chain["app"] = "yapchain-localnet-1"
        # without an expected chain id: read-only works with a warning, never silently adopts the node's
        os.remove(os.path.join(home, "network.json"))
        r = run(yap, home, "network", "--rpc", m.url, "status", "--json")
        check(r.returncode == 0 and "chain identity not checked" in r.stderr, "warning without chain id")
        check(json.loads(r.stdout)["chain_id_verified"] is False, "not verified without chain id")
        check(not os.path.exists(os.path.join(home, "network.json")), "status must not write the config")
        # 5. JSON errors are reported as JSON with the exit code
        r = run(yap, home, "network", "--rpc", "http://127.0.0.1:1", "status", "--json")
        j = json.loads(r.stdout)
        check(r.returncode == 6 and j["exit_code"] == 6 and "connect" in j["error"], f"json error {r.stdout}")
        # 6. local commands ignore the network configuration entirely (dev mode stays local)
        before = len(m.requests)
        r = run(yap, home, "query", "chain", env={"YAP_RPC": m.url})
        check(r.returncode != 0, "local query without a local chain must fail")
        run(yap, home, "query", "chain", env={"YAP_RPC": m.url, "YAP_CHAIN_ID": "yapchain-localnet-1"})
        check(len(m.requests) == before, "a local command contacted the RPC")
        # 7. no secret material in requests (none exists here, but the headers stay minimal)
        hdrs = {k.lower() for r in m.requests for k in r["headers"]}
        check(not ({"authorization", "cookie", "proxy-authorization"} & hdrs), f"headers {hdrs}")
    if FAILS:
        print(f"{len(FAILS)} failure(s)", file=sys.stderr)
        return 1
    print("network config and chain identity: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(os.path.abspath(sys.argv[1])))
