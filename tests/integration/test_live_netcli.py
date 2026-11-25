#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Live E2E of the network CLI (M2.6f): four validators + one non-validator full node (real CometBFT
v0.40.0 + yapd), every wallet operation through real `yap` subprocesses (`yap network ...`, `yap keys`,
offline `yap tx`). The independent state model of livenet.py checks every node at every answer.

    test_live_netcli.py <yap> <yapd> <cometbft>
"""
import base64
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "cli"))
from livenet import *  # noqa: E402,F401,F403
from livenet import ln  # noqa: E402
from mock_rpc import MockRpc, status_result  # noqa: E402

CHAIN = "yapchain-localnet-1"


class Cli:
    """`yap` as a subprocess with a given home and password file; every call is recorded."""

    def __init__(self, yap, home, password_file, log):
        self.yap, self.home, self.pw, self.log = yap, home, password_file, log

    def __call__(self, *args, rpc=None, chain=CHAIN, net=True, timeout=180):
        cmd = [self.yap, "--home", self.home, "--password-file", self.pw]
        if net:
            cmd += ["network"] + (["--rpc", rpc] if rpc else []) + (["--chain-id", chain] if chain else [])
        cmd += list(args)
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, stdin=subprocess.DEVNULL)
        self.log.write(json.dumps({"cmd": cmd[1:], "rc": r.returncode, "out": r.stdout, "err": r.stderr}) + "\n")
        return r

    def json(self, *args, **kw):
        r = self(*args, "--json", **kw)
        try:
            return r, json.loads(r.stdout)
        except ValueError:
            raise Fail(f"{args}: not JSON (rc {r.returncode}): {r.stdout!r} {r.stderr!r}")


def write_private(path, text):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    os.write(fd, text.encode())
    os.close(fd)


def tree_digest(path):
    h = hashlib.sha256()
    for root, _, files in sorted(os.walk(path)):
        for f in sorted(files):
            p = os.path.join(root, f)
            h.update(p.encode())
            h.update(open(p, "rb").read())
    return h.hexdigest()


def main(yap, yapd, cometbft):
    base = os.environ.get("YAP_LIVE_DIR") or tempfile.mkdtemp(prefix="yc-")
    d = os.path.join(base, "net")
    net = None
    ok = False
    results = []

    def done(n, text):
        results.append((n, text))
        note(f"[{n}] {text}")

    try:
        net = Net(d, yap, yapd, cometbft)
        model = Model(net)
        v = net.validators
        for n in v:
            n.start()
        net.wait_height(3)
        fn = net.add_full_node()
        fn.start()
        wait_until(lambda: fn.height() >= net.min_height(v) and not fn.rpc("status")["sync_info"]["catching_up"],
                   120, "full node sync")
        full = fn.meta["rpc"]
        rpcs = [n.meta["rpc"] for n in net.nodes]
        log = open(os.path.join(base, "cli-calls.jsonl"), "w", buffering=1)

        # Genesis wallet (alice, bob, ...) and a fresh user wallet.
        gw = Cli(yap, net.info["wallet_home"], net.info["password_file"], log)
        uhome = os.path.join(base, "user")
        os.makedirs(uhome, mode=0o700)
        upw = os.path.join(base, "user.password")
        user_password = "user-pass-" + hashlib.sha256(base.encode()).hexdigest()[:24]
        write_private(upw, user_password + "\n")
        u = Cli(yap, uhome, upw, log)
        alice = net.addresses["alice"]

        def sent(kind, sender_addr, amount, to_addr, out):
            hx = open(out).read().strip()
            model.register_raw(hx, kind, sender_addr, amount, FEE, to_addr)
            return hashlib.sha256(bytes.fromhex(hx)).hexdigest().upper()

        step("N1", "create local wallets")
        r1 = u("keys", "create", "w1", net=False)
        r2 = u("keys", "create", "w2", net=False)
        check(r1.returncode == 0 and r2.returncode == 0, f"keys create {r1.stderr} {r2.stderr}")
        w1 = ln.kv(r1.stdout)["address"]
        w2 = ln.kv(r2.stdout)["address"]
        rc = u("config", "set", "--rpc", full, "--chain-id", CHAIN, chain=None)
        check(rc.returncode == 0, f"config set: {rc.stderr}")
        done(1, f"wallets w1={w1[:16]}…, w2={w2[:16]}…; network config -> full node RPC")

        step("N2", "remote balance and nonce")
        r, ja = gw.json("query", "account", alice, rpc=full)
        check(r.returncode == 0 and ja["exists"] and ja["nonce"] == "0", f"alice {ja}")
        r, j = u.json("query", "account", w1)
        check(r.returncode == 0 and j["exists"] is False and j["balance_yip"] == "0", f"w1 absent {j}")
        done(2, f"alice balance {ja['balance_yip']} yip, nonce {ja['nonce']}; w1 absent (exists=false, exit 0)")

        step("N3-N4", "transfer through the full node's RPC; confirm finalization")
        out = os.path.join(base, "t1.hex")
        r, j = gw.json("transfer", "--from", "alice", "--to", w1, "--amount", "10YAP", "--yes", "--out", out, rpc=full)
        check(r.returncode == 0 and j["status"] == "accepted_mempool", f"transfer {r.stdout} {r.stderr}")
        h1 = sent("transfer", alice, 10 * YIP, w1, out)
        check(j["hash"] == h1, "hash computed locally")
        r, j = u.json("wait", h1, "--timeout", "60")
        check(r.returncode == 0 and j["status"] == "finalized" and j["result"] == "OK", f"wait {r.stdout}")
        hh = int(j["height"])
        done(3, f"accepted_mempool via full node, hash {h1[:16]}…")
        done(4, f"finalized at height {hh}, code 0")

        step("N5", "balances and nonce on all five nodes")
        net.wait_height(hh, net.nodes)
        for rp in rpcs:
            _, a = u.json("query", "account", w1, rpc=rp)
            _, b = u.json("query", "account", alice, rpc=rp)
            check(a["balance_yip"] == str(10 * YIP) and b["nonce"] == "1", f"{rp}: w1 {a} alice {b}")
        for n in net.nodes:
            check_node_state(net, model, n, all_accounts(net) + [w1, w2])
        done(5, "w1 = 10 YAP, alice nonce 1 on all five RPCs; state exact against the model on every node")

        step("N6", "burn and supply")
        out = os.path.join(base, "b1.hex")
        _, c0 = u.json("query", "chain")
        r, j = u.json("burn", "--from", "w1", "--amount", "1YAP", "--yes", "--wait", "--out", out)
        check(r.returncode == 0 and j["status"] == "finalized", f"burn {r.stdout} {r.stderr}")
        sent("burn", w1, YIP, None, out)
        _, c1 = u.json("query", "chain")
        check(int(c1["total_burned_yip"]) - int(c0["total_burned_yip"]) == YIP + FEE // 2, f"burned {c0} {c1}")
        check(int(c1["total_supply_yip"]) == int(c1["total_minted_yip"]) - int(c1["total_burned_yip"]), "supply")
        done(6, f"burned +{YIP + FEE // 2} yip (1 YAP + half the fee); supply = minted - burned")

        step("N7", "offline sign, then broadcast")
        _, acc = u.json("query", "account", w1)
        _, st = u.json("status")
        tip = int(st["latest_height"])
        off = os.path.join(base, "off.hex")
        r = u("tx", "transfer", "--from", "w1", "--to", w2, "--amount", "2YAP", "--nonce", acc["nonce"],
              "--expires", str(tip + 100), "--chain-id", CHAIN, "--out", off, "--yes", net=False)
        check(r.returncode == 0, f"offline sign {r.stderr}")
        h7 = sent("transfer", w1, 2 * YIP, w2, off)
        r, j = u.json("send", off, "--wait")
        check(r.returncode == 0 and j["status"] == "finalized" and j["hash"] == h7, f"send {r.stdout}")
        done(7, f"signed offline (no network), sent and finalized at {j['height']}")

        step("N8", "resending an executed transaction")
        # Whether the node answers from its cache or with CheckTx ERR_NONCE, the CLI looks the hash up and
        # reports the earlier execution — never "rejected"/"not executed" for an executed transaction.
        r, j = u.json("send", off, rpc=rpcs[1])
        check(r.returncode == 0 and j["status"] == "finalized", f"resend {r.stdout}")
        r2_, j2 = u.json("send", off, rpc=rpcs[2])
        check(r2_.returncode == 0 and j2["status"] == "finalized", f"resend {r2_.stdout}")
        done(8, f"resend -> {j['status']} (exit {r.returncode}) / {j2['status']} (exit {r2_.returncode}); "
                "executed once (model)")

        step("N9", "two conflicting transactions with the same nonce")
        _, acc = u.json("query", "account", w1)
        _, st = u.json("status")
        exp9 = int(st["latest_height"]) + 6
        files = []
        for k, amt in enumerate(("1", "2")):
            f = os.path.join(base, f"c{k}.hex")
            u("tx", "transfer", "--from", "w1", "--to", w2, "--amount", amt + "YAP", "--nonce", acc["nonce"],
              "--expires", str(exp9), "--chain-id", CHAIN, "--out", f, "--yes", net=False)
            files.append((f, sent("transfer", w1, int(amt) * YIP, w2, f)))
        sends = parallel(lambda: u.json("send", files[0][0], rpc=rpcs[0]), lambda: u.json("send", files[1][0], rpc=rpcs[3]))
        waits = [u.json("wait", hx, "--expires", str(exp9), "--timeout", "90") for _, hx in files]
        outcomes = sorted(w[1]["status"] for w in waits)
        check(outcomes == ["finalized", "unknown"], f"conflict outcomes {outcomes} (sends {[s[1]['status'] for s in sends]})")
        check(sorted(w[0].returncode for w in waits) == [0, 8], "exit codes 0 and 8")
        loser = next(w[1] for w in waits if w[1]["status"] == "unknown")
        check(loser.get("inclusion_window") == "closed" and loser.get("execution") == "unknown",
              f"loser: window closed, execution unknown (never 'not executed'): {loser}")
        done(9, f"sent {[s[1]['status'] for s in sends]}; outcomes {outcomes} (exit 0 / 8: window closed, "
                "execution unknown)")

        step("N10", "wrong chain id")
        r = u("status", chain="other-chain")
        wrong = os.path.join(base, "wrong.hex")
        u("tx", "burn", "--from", "w1", "--amount", "1", "--nonce", "99", "--expires", "999", "--chain-id",
          "other-chain", "--out", wrong, "--yes", net=False)
        r2_ = u("send", wrong)
        check(r.returncode == 10 and r2_.returncode == 10, f"chain mismatch {r.returncode} {r2_.returncode}")
        done(10, "node/app identity mismatch -> 10; other-chain transaction refused locally -> 10 (not sent)")

        step("N11", "invalid signature")
        badf = os.path.join(base, "bad.hex")
        raw = bytearray(bytes.fromhex(open(off).read().strip()))
        raw[-1] ^= 1
        open(badf, "w").write(raw.hex())
        r = u("send", badf)
        r2_, j2 = u.json("send", badf, "--skip-signature-check")
        check(r.returncode == 2 and r2_.returncode == 2 and j2["result"] == "ERR_SIGNATURE", f"bad sig {r2_.stdout}")
        done(11, "refused locally (exit 2); with --skip-signature-check the node's CheckTx says ERR_SIGNATURE (exit 2)")

        step("N12", "insufficient balance")
        r, j = u.json("transfer", "--from", "w2", "--to", w1, "--amount", "1000000YAP", "--yes")
        check(r.returncode == 2 and j["result"] == "ERR_INSUFFICIENT_FUNDS", f"insufficient {r.stdout}")
        done(12, "CheckTx ERR_INSUFFICIENT_FUNDS -> exit 2")

        step("N13", "expired expiry height")
        _, acc2 = u.json("query", "account", w2)
        _, st = u.json("status")
        old = os.path.join(base, "old.hex")
        u("tx", "transfer", "--from", "w2", "--to", w1, "--amount", "1", "--nonce", acc2["nonce"], "--expires",
          str(int(st["latest_height"]) - 1), "--chain-id", CHAIN, "--out", old, "--yes", net=False)
        r, j = u.json("send", old)
        check(r.returncode == 2 and j["result"] == "ERR_EXPIRED", f"expired {r.stdout}")
        done(13, "CheckTx ERR_EXPIRED -> exit 2")

        step("N14", "quorum lost after the transaction entered the mempool")
        out = os.path.join(base, "q.hex")
        for n in (v[2], v[3]):
            n.stop()
        stall = net.min_height([v[0], v[1], fn])
        r, j = gw.json("transfer", "--from", "bob", "--to", w2, "--amount", "3YAP", "--yes", "--out", out, rpc=full)
        hq = sent("transfer", net.addresses["bob"], 3 * YIP, w2, out)
        check(r.returncode == 0 and j["status"] == "accepted_mempool", f"accepted without quorum {r.stdout}")
        done(14, f"two validators stopped at height ~{stall}; transfer accepted into the mempool (not final)")

        step("N15", "a wait timeout is not a rejection")
        t0 = time.time()
        r, j = u.json("wait", hq, "--timeout", "10")
        check(r.returncode == 8 and j["status"] == "unknown" and "NOT mean rejected" in j["note"], f"timeout {r.stdout}")
        check("consensus may be stalled" in r.stderr, "stall note")
        check(time.time() - t0 < 25, "bounded wait")
        done(15, "exit 8 status unknown ('does NOT mean rejected'), stall noted, bounded")

        step("N16", "quorum back; the result by hash")
        for n in (v[2], v[3]):
            n.start()
        r, j = u.json("wait", hq, "--timeout", "120")
        check(r.returncode == 0 and j["status"] == "finalized", f"after recovery {r.stdout}")
        # validator-3 has just restarted and is catching up: its index can lag (the reason `tx` must never
        # be read as "not executed"), so it is asked with `wait`, which tolerates a lagging index.
        r2_, j2 = u.json("wait", hq, "--timeout", "60", rpc=rpcs[3])
        check(r2_.returncode == 0 and j2["height"] == j["height"], f"same result from another node: {r2_.stdout}")
        done(16, f"finalized at {j['height']} after recovery; same answer from validator-3")

        step("N17", "RPC endpoint down")
        fn.stop()
        r = u("status")
        r2_ = u("send", off)
        check(r.returncode == 6 and r2_.returncode == 6 and "not sent" in r2_.stderr, f"down {r.returncode} {r2_.stderr}")
        fn.start()
        wait_until(lambda: fn.height() >= net.min_height(v), 120, "full node back")
        done(17, "status -> 6; send -> 6 'the transaction was not sent'")

        step("N18", "malformed / oversized RPC answers and TLS verification")
        def bad_handler(method, p):
            if mode["m"] == "garbage":
                return ("raw", 200, b"\x00\xff{{not json")
            if mode["m"] == "huge":
                return ("huge", 64 << 20)
            return ("result", status_result(CHAIN))
        mode = {"m": "garbage"}
        with MockRpc(bad_handler) as mock:
            r = u("status", rpc=mock.url)
            mode["m"] = "huge"
            t0 = time.time()
            r2_ = u("status", "--max-response-bytes", "1048576", rpc=mock.url)
            dt = time.time() - t0
        check(r.returncode == 6 and "malformed" in r.stderr, f"garbage {r.stderr}")
        check(r2_.returncode == 6 and "larger than" in r2_.stderr and dt < 20, f"huge {r2_.stderr} {dt}")
        tls = tls_check(u, base)
        done(18, f"garbage -> 6; 64 MiB answer cut at 1 MiB -> 6 in {dt:.1f}s; {tls}")

        step("N19", "no secrets in any log or CLI output")
        blob = open(os.path.join(base, "cli-calls.jsonl")).read()
        check(user_password not in blob and open(net.info["password_file"]).read().strip() not in blob,
              "a password appears in CLI output")
        for n in net.nodes:
            for name in ("yapd", "cometbft"):
                text = n.log(name)
                check(user_password not in text, f"{n}/{name}.log has the user password")
        check_no_secrets(net)
        done(19, "no wallet password, validator or node key in node logs or CLI output")

        step("N20", "read-only commands change nothing")
        start = max(n.height() for n in net.nodes)   # every block after this one is checked
        digest0 = tree_digest(uhome)
        nonces0 = {a: u.json("query", "account", a)[1]["nonce"] for a in (alice, w1, w2)}
        for rp in rpcs:
            for args in (["status"], ["info"], ["query", "chain"], ["query", "params"], ["query", "balance", w1],
                         ["query", "account", w2], ["query", "block"], ["tx", hq], ["config", "show"]):
                check(u(*args, rpc=rp).returncode == 0, f"read-only {args} on {rp}")
        end = net.wait_height(max(n.height() for n in net.nodes) + 2, net.nodes)
        check(end > start + 1, "no block window")
        txs = sum(len(fn.block(h)["block"]["data"]["txs"] or []) for h in range(start + 1, end + 1))
        nonces1 = {a: u.json("query", "account", a)[1]["nonce"] for a in (alice, w1, w2)}
        check(txs == 0 and nonces0 == nonces1 and tree_digest(uhome) == digest0, f"state changed: {txs} txs")
        done(20, f"{9 * len(rpcs)} read-only calls: no transaction in blocks {start + 1}..{end}, nonces and the "
                 "wallet directory unchanged")

        step("Z", "all five nodes agree; exact state; verify-state")
        tip = net.wait_height(net.min_height(net.nodes) + 2, net.nodes)
        check_same_chain(net, net.nodes, tip - 1)
        for n in net.nodes:
            check_node_state(net, model, n, all_accounts(net) + [w1, w2])
        codes = net.stop_all()
        check(all(c.get("cometbft", 0) == 0 and c.get("yapd", 0) == 0 for c in codes.values()), f"exit codes {codes}")
        for n in net.nodes:
            vs = ln.kv(net.cli("verify-state", home=n.meta["yapd_home"]).stdout)
            check(int(vs["height"]) == n.commits()[-1][0], f"{n}: verify-state {vs}")
        print("NETWORK CLI RESULTS " + json.dumps(results), flush=True)
        ok = True
        print("NETWORK CLI LIVE E2E PASSED")
        return 0
    except (Fail, ln.LocalnetError) as e:
        print(f"FAILED: {e}", file=sys.stderr)
        return 1
    finally:
        if net is not None:
            try:
                net.stop_all()
            except Exception as e:  # noqa: BLE001
                print(f"cleanup: {e}", file=sys.stderr)
        ln.kill_all(d)
        if ok and not os.environ.get("YAP_LIVE_KEEP"):
            shutil.rmtree(base, ignore_errors=True)
        else:
            print(f"artifacts kept in {base}", file=sys.stderr)


def tls_check(u, base):
    """https endpoint with a self-signed certificate: refused by default, accepted with --ca-file."""
    import ssl
    cert, key = os.path.join(base, "tls.crt"), os.path.join(base, "tls.key")
    r = subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                        "-nodes", "-keyout", key, "-out", cert, "-days", "1", "-subj", "/CN=localhost",
                        "-addext", "subjectAltName=DNS:localhost"], capture_output=True, text=True)
    if r.returncode != 0:
        raise Fail(f"openssl: {r.stderr}")
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    from mock_rpc import chain_answer, query_result

    def h(method, p):
        if method == "status":
            return ("result", status_result(CHAIN))
        return ("result", query_result(chain_answer(CHAIN)))
    with MockRpc(h, tls_context=ctx) as m:
        r1 = u("status", rpc=m.url)
        r2 = u("status", "--ca-file", cert, rpc=m.url)
    check(r1.returncode == 6 and "TLS" in r1.stderr, f"self-signed accepted: {r1.returncode} {r1.stderr}")
    check(r2.returncode == 0, f"--ca-file refused: {r2.stderr}")
    return "https with a self-signed certificate refused (6), accepted with --ca-file"


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(main(*[os.path.abspath(p) for p in sys.argv[1:]]))
