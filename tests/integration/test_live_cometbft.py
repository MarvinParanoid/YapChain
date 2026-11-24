#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Live end-to-end test: real CometBFT v0.40.0 + yapd + the real `yap` CLI (M2.3).

Single validator, loopback only — not a BFT network. Everything goes through processes and the
CometBFT RPC; no library code is called. Expected amounts are computed here from the protocol rules
(protocol.md §9.3, §9.5; economics.md §8), system-account addresses from the independent Python
reference (tools/refgen).

    test_live_cometbft.py <yap> <yapd> <cometbft> <yapd-test-hooks>

The crash scenarios run the separate test build yapd-test-hooks (the only binary that honours
YAPD_TEST_CRASH); every other start, including the recovery after each crash, uses yapd.

Scenario (numbered steps are printed):
  1-17  start, InitChain, blocks, Transfer through RPC, balances/emission/fees, app_hash vs state
        root, invalid transactions, stop, restart, handshake, continuation, no double emission,
        verify-state;
  C1    yapd killed when Commit of height H arrives (after FinalizeBlock, before the durable write);
  C2    yapd killed after the durable Commit of height H, before CometBFT receives the response.
"""
import base64
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts", "localnet"))
sys.path.insert(0, os.path.join(ROOT, "tools", "refgen"))
import localnet as ln  # noqa: E402
import bech32m_ref  # noqa: E402

YIP = 1_000_000
GENESIS = {"alice": 1_000_000 * YIP, "bob": 1_000 * YIP}
G_SUPPLY = sum(GENESIS.values())
REWARD, V_SHARE, POOL_SHARE, T_SHARE = 2_000_000, 1_200_000, 600_000, 200_000  # economics.md §8
FEE = 1_000
AMOUNT = 1_500_000  # 1.5 YAP
ERR_DECODE, ERR_NONCE, ERR_INSUFFICIENT_FUNDS = 1, 10, 11


class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


def step(n, text):
    print(f"[{n}] {text}", flush=True)


def system_address(name):
    t = b"YAP/sys-account/v1"
    return bech32m_ref.encode_address("tyap", hashlib.sha256(bytes([len(t)]) + t + name).digest())


TREASURY = system_address(b"treasury")
POOL = system_address(b"rewards-pool")


class Net:
    def __init__(self, d, yap, yapd, cometbft, yapd_hooks):
        self.d, self.yap, self.yapd, self.cometbft = d, yap, yapd, cometbft
        self.yapd_hooks = yapd_hooks
        self.meta = None
        self.procs = {}

    # ---- processes
    def start(self, crash=None, wait=2):
        env = {"YAPD_TEST_CRASH": crash} if crash else {}
        self.procs["yapd"] = ln.start_yapd(self.d, self.yapd_hooks if crash else self.yapd, env)
        self.procs["cometbft"] = ln.start_cometbft(self.d, self.cometbft)
        if wait:
            return ln.wait_height(self.d, wait, timeout=60)
        return None

    def stop(self):
        """CometBFT first, then yapd (SIGTERM). Returns the exit status of each process."""
        codes = {}
        for name in ("cometbft", "yapd"):
            p = self.procs.pop(name, None)
            if p is None:
                continue
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
            codes[name] = wait_exit(p, 60, name)
            pid_file = os.path.join(self.d, f"{name}.pid")
            if os.path.exists(pid_file):
                os.remove(pid_file)
        return codes

    # ---- RPC
    def rpc(self, method, **params):
        return ln.rpc(self.meta, method, **params)

    def height(self):
        return ln.height(self.meta)

    def query(self, path):
        r = self.rpc("abci_query", path=f'"{path}"')["response"]
        value = json.loads(base64.b64decode(r["value"])) if r.get("value") else None
        return int(r.get("code", 0)), int(r.get("height", 0)), value

    def header(self, h):
        return self.rpc("block", height=h)["block"]["header"]

    def broadcast(self, tx_hex):
        return self.rpc("broadcast_tx_sync", tx="0x" + tx_hex)

    # ---- CLI
    def cli(self, *args, home=None, check_rc=True):
        cmd = [self.yap, "--home", home or self.meta["wallet_home"], "--password-file", self.meta["password_file"], *args]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if check_rc and r.returncode != 0:
            raise Fail(f"{' '.join(cmd)} -> {r.returncode}\n{r.stdout}{r.stderr}")
        return r

    def offline_chain(self):
        """`yap query chain` on yapd's own database (read-only; yapd must be stopped or idle)."""
        return ln.kv(self.cli("query", "chain", home=self.meta["yapd_home"]).stdout)

    def commits_in_log(self):
        """Every `Commit: height H, app_hash X` that yapd logged, in order."""
        text = open(os.path.join(self.d, "yapd.log")).read()
        return [(int(h), r) for h, r in re.findall(r"Commit: height (\d+), app_hash ([0-9a-f]{64})", text)]


def expected(h, burned, alice_out, bob_in):
    """Committed state at height h (one validator that signs every block from height 2)."""
    return {
        "total_minted": G_SUPPLY + REWARD * h,
        "total_burned": burned,
        "total_supply": G_SUPPLY + REWARD * h - burned,
        "alice": GENESIS["alice"] - alice_out,
        "bob": GENESIS["bob"] + bob_in,
        "validator-reward": V_SHARE * (h - 1),  # height 1: empty last_commit, share goes to treasury
        POOL: POOL_SHARE * h,
        TREASURY: T_SHARE * h + V_SHARE + (burned and FEE - burned),
    }


def check_economics(net, burned, alice_out, bob_in, alice_nonce):
    """Each query is answered at one committed height; every value is checked at that height."""
    _, h, chain = net.query("/chain")
    exp = expected(h, burned, alice_out, bob_in)
    for k in ("total_minted", "total_burned", "total_supply"):
        check(int(chain[k]) == exp[k], f"/chain {k} at height {h}: {chain[k]} != {exp[k]}")
    for name in ("alice", "bob", "validator-reward"):
        _, ha, acc = net.query("/account/" + net.meta["addresses"][name])
        e = expected(ha, burned, alice_out, bob_in)[name]
        check(int(acc["balance"]) == e, f"{name} balance at height {ha}: {acc['balance']} != {e}")
    for addr in (POOL, TREASURY):
        _, ha, acc = net.query("/account/" + addr)
        e = expected(ha, burned, alice_out, bob_in)[addr]
        check(int(acc["balance"]) == e, f"{addr} balance at height {ha}: {acc['balance']} != {e}")
    _, _, acc = net.query("/account/" + net.meta["addresses"]["alice"])
    check(int(acc["nonce"]) == alice_nonce, f"alice nonce {acc['nonce']} != {alice_nonce}")
    return h


def check_app_hashes(net):
    """protocol semantics: the app_hash of height H is in the header of H+1. Every height yapd ever
    committed appears exactly once (no height applied twice, none skipped)."""
    commits = net.commits_in_log()
    heights = [h for h, _ in commits]
    check(heights == list(range(1, len(heights) + 1)),
          f"committed heights are not exactly 1..{len(heights)} once each: {heights}")
    tip = net.height()
    checked = 0
    for h, root in commits:
        if h + 1 <= tip:
            got = net.header(h + 1)["app_hash"].lower()
            check(got == root, f"header {h + 1} app_hash {got} != state root of height {h} {root}")
            checked += 1
    return checked


def make_transfer(net, nonce, amount, out, sender="alice", to="bob", expires=None):
    expires = expires or net.height() + 100
    r = net.cli("tx", "transfer", "--from", sender, "--to", net.meta["addresses"][to], "--amount", str(amount),
                "--fee", str(FEE), "--nonce", str(nonce), "--expires", str(expires),
                "--chain-id", net.meta["chain_id"], "--out", out, "--yes")
    return open(out).read().strip(), ln.kv(r.stdout)["tx_id"]


def wait_tx(net, cmt_hash, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            return net.rpc("tx", hash="0x" + cmt_hash)
        except ln.LocalnetError:
            time.sleep(0.3)
    raise Fail(f"transaction {cmt_hash} not included within {timeout}s")


def wait_exit(p, timeout, what):
    try:
        return p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        raise Fail(f"{what} did not exit within {timeout}s")


def crash_scenario(net, label, point, burned, alice_out, bob_in, nonce):
    """Stops the network, restarts yapd armed to SIGKILL itself at `point` of height H, then restarts
    everything normally and checks recovery."""
    net.stop()
    before = int(net.offline_chain()["height"])
    target = before + 3
    step(label, f"restart with YAPD_TEST_CRASH={point}:{target} (committed height {before})")
    net.start(crash=f"{point}:{target}", wait=0)
    rc = wait_exit(net.procs["yapd"], 60, "yapd (crash point)")
    check(rc == -9, f"yapd should die by SIGKILL at {point}:{target}, exit status {rc}")
    # CometBFT notices the dead application and stops by itself; give it time, then make sure.
    try:
        cmt_rc = net.procs["cometbft"].wait(timeout=30)
        cmt_note = f"CometBFT stopped by itself (status {cmt_rc})"
    except subprocess.TimeoutExpired:
        cmt_note = "CometBFT kept running and was stopped with SIGTERM"
    net.stop()
    off = net.offline_chain()
    want = target - 1 if point == "before_commit" else target
    check(int(off["height"]) == want, f"{point}: durable height {off['height']} != {want}")
    step(label, f"yapd killed at {point}:{target}; durable height {off['height']}; {cmt_note}")

    log_mark = os.path.getsize(os.path.join(net.d, "cometbft.log"))
    net.start(wait=target + 3)
    cmt_log = open(os.path.join(net.d, "cometbft.log")).read()[log_mark:]
    replay = "Replay last block using real app" if point == "before_commit" else "Replay last block using mock app"
    check(replay in cmt_log, f"CometBFT did not '{replay}' after {point}")
    check("Completed ABCI Handshake" in cmt_log, "handshake did not complete after the crash")
    n = check_app_hashes(net)
    h = check_economics(net, burned, alice_out, bob_in, nonce)
    step(label, f"recovered: '{replay}', chain continued to height {h}, {n} app_hashes match, no height twice")


def main(yap, yapd, cometbft, yapd_hooks):
    base = os.environ.get("YAP_LIVE_DIR") or tempfile.mkdtemp(prefix="yapnet-")
    d = os.path.join(base, "net")
    net = Net(d, yap, yapd, cometbft, yapd_hooks)
    ok = False
    try:
        net.meta = ln.init(d, yap, cometbft)
        a = net.meta["addresses"]

        step(1, "start yapd")
        net.procs["yapd"] = ln.start_yapd(d, yapd)
        step(2, "start CometBFT v0.40.0")
        net.procs["cometbft"] = ln.start_cometbft(d, cometbft)

        step(3, "InitChain: real request accepted by every §11.4 check; header(1).app_hash = genesis state root")
        ln.wait_height(d, 1)
        ylog = open(os.path.join(d, "yapd.log")).read()
        m = re.search(r"ABCI InitChain chain_id=(\S+) initial_height=(\d+) validators=(\d+) app_version=(\d+) "
                      r"vote_ext_height=(\d+)", ylog)
        check(m and m.groups() == (net.meta["chain_id"], "1", "1", "1", "0"), f"InitChain request: {m and m.groups()}")
        root0 = re.search(r"InitChain: genesis ([0-9a-f]{64}), app_hash ([0-9a-f]{64})", ylog)
        check(root0 is not None, "InitChain was not logged")
        check(root0.group(1) == net.meta["genesis_hash"], "genesis hash differs from `yap genesis hash`")
        # Independent genesis root: the same genesis loaded by the CLI into a scratch dev chain.
        scratch = os.path.join(base, "devroot")
        net.cli("dev", "init", "--genesis", net.meta["yap_genesis"], home=scratch)
        dev_root = ln.kv(net.cli("query", "chain", home=scratch).stdout)["state_root"]
        check(dev_root == root0.group(2), f"InitChain app_hash {root0.group(2)} != CLI genesis root {dev_root}")
        check(net.header(1)["app_hash"].lower() == root0.group(2), "header(1).app_hash != InitChain app_hash")
        check(net.header(1)["version"]["app"] == "1", "block version.app != 1")
        g_time = ln.rfc3339_ms(net.meta["genesis_time_ms"]).replace(".000Z", "Z")
        check(net.header(1)["time"] == g_time, f"block 1 time {net.header(1)['time']} != genesis time {g_time}")

        step(4, "several blocks")
        h = ln.wait_height(d, 4)
        info = net.rpc("abci_info")["response"]
        check(info["data"] == "yapchain" and info["app_version"] == "1", f"abci_info {info}")

        step(5, "create and sign a Transfer with the real CLI (nonce from abci_query)")
        _, _, acc = net.query("/account/" + a["alice"])
        check(acc["nonce"] == "0", "alice nonce before transfer")
        tx_hex, tx_id = make_transfer(net, 0, AMOUNT, os.path.join(base, "t1.hex"))

        step(6, "submit through the CometBFT RPC (broadcast_tx_sync)")
        r = net.broadcast(tx_hex)
        check(r["code"] == 0, f"CheckTx rejected the transfer: {r}")
        cmt_hash = r["hash"]
        check(cmt_hash.lower() == hashlib.sha256(bytes.fromhex(tx_hex)).hexdigest(), "CometBFT tx hash")

        step(7, "wait for inclusion")
        t = wait_tx(net, cmt_hash)
        tx_height = int(t["height"])
        check(t["tx_result"].get("code", 0) == 0, f"tx result {t['tx_result']}")
        blk = net.rpc("block", height=tx_height)["block"]
        check(base64.b64decode(blk["data"]["txs"][0]).hex() == tx_hex, "block does not carry the exact tx bytes")
        results = net.rpc("block_results", height=tx_height)
        check(len(results["txs_results"]) == len(blk["data"]["txs"]), "one result per tx")
        print(f"    included at height {tx_height}, yap tx_id {tx_id}, CometBFT hash {cmt_hash}")

        step(8, "balances and nonce")
        step(9, "emission and fees (exact: minted, burned, supply, validator, pool, treasury)")
        burned = FEE // 2  # fee_burn_bps = 5000
        h = check_economics(net, burned, AMOUNT + FEE, AMOUNT, 1)
        print(f"    economics exact at height {h}")

        step(10, "app_hash of height H (header H+1) = yapd state root of H, for every height")
        ln.wait_height(d, h + 2)
        n = check_app_hashes(net)
        _, qh, chain = net.query("/chain")
        nh = ln.wait_height(d, qh + 1)
        check(net.header(qh + 1)["app_hash"].lower() == chain["state_root"], "Query state_root vs header")
        print(f"    {n} heights checked; /chain at {qh} root {chain['state_root'][:16]}… (tip {nh})")

        step(11, "invalid transactions are rejected by CheckTx")
        # The exact same bytes never reach the application: CometBFT's mempool cache refuses them.
        try:
            net.broadcast(tx_hex)
            raise Fail("an exact replay was accepted by broadcast_tx_sync")
        except ln.LocalnetError as e:
            check("tx already exists in cache" in str(e), f"exact replay: {e}")
        # A different transaction with the already used nonce 0 reaches CheckTx: ERR_NONCE.
        stale_hex, _ = make_transfer(net, 0, AMOUNT + 1, os.path.join(base, "t1b.hex"))
        replay = net.broadcast(stale_hex)
        check(replay["code"] == ERR_NONCE, f"stale nonce: {replay}")
        garbage = net.broadcast("00010203")
        check(garbage["code"] == ERR_DECODE, f"garbage: {garbage}")
        too_much, _ = make_transfer(net, 0, 2 * GENESIS["bob"], os.path.join(base, "t2.hex"), sender="bob", to="alice")
        poor = net.broadcast(too_much)
        check(poor["code"] == ERR_INSUFFICIENT_FUNDS, f"insufficient funds: {poor}")

        step(12, "no state change from them")
        ln.wait_height(d, net.height() + 2)
        for bad in (replay, garbage, poor):
            try:
                net.rpc("tx", hash="0x" + bad["hash"])
                raise Fail(f"rejected tx {bad['hash']} was included")
            except ln.LocalnetError:
                pass
        check(int(net.rpc("num_unconfirmed_txs")["n_txs"]) == 0, "mempool not empty")
        check_economics(net, burned, AMOUNT + FEE, AMOUNT, 1)
        _, _, bob = net.query("/account/" + a["bob"])
        check(bob["nonce"] == "0", "bob nonce changed")

        step(13, "graceful stop of CometBFT and yapd")
        codes = net.stop()
        check(codes == {"cometbft": 0, "yapd": 0}, f"exit codes after SIGTERM: {codes}")
        stop_tail = open(os.path.join(d, "yapd.log")).read().strip().splitlines()[-1]
        check("stopped at height" in stop_tail, f"yapd did not stop cleanly: {stop_tail}")
        off = net.offline_chain()
        stopped_at = int(off["height"])
        print(f"    stopped at height {stopped_at}, root {off['state_root'][:16]}…, exit codes {codes}")

        step(14, "restart with the existing data")
        mark = os.path.getsize(os.path.join(d, "cometbft.log"))
        ymark = os.path.getsize(os.path.join(d, "yapd.log"))
        net.start(wait=stopped_at + 3)

        step(15, "Info, handshake, continuation")
        cmt_log = open(os.path.join(d, "cometbft.log")).read()[mark:]
        m = re.search(r"ABCI Handshake App Info\s+module=consensus height=(\d+) hash=([0-9A-F]+)", cmt_log)
        check(m and int(m.group(1)) == stopped_at and m.group(2).lower() == off["state_root"],
              f"handshake Info: {m and m.groups()} vs {stopped_at} {off['state_root']}")
        check("Completed ABCI Handshake" in cmt_log, "handshake not completed")
        check(net.header(stopped_at + 1)["app_hash"].lower() == off["state_root"], "first block after restart")

        step(16, "no double emission and no second InitChain")
        ylog = open(os.path.join(d, "yapd.log")).read()[ymark:]
        check("ABCI InitChain" not in ylog, "InitChain was sent again after restart")
        check_app_hashes(net)
        h = check_economics(net, burned, AMOUNT + FEE, AMOUNT, 1)
        print(f"    economics exact at height {h} after restart")

        crash_scenario(net, "C1", "before_commit", burned, AMOUNT + FEE, AMOUNT, 1)
        crash_scenario(net, "C2", "after_commit", burned, AMOUNT + FEE, AMOUNT, 1)

        step(17, "verify-state on the stopped node")
        codes = net.stop()
        check(codes == {"cometbft": 0, "yapd": 0}, f"exit codes after SIGTERM: {codes}")
        r = net.cli("verify-state", home=net.meta["yapd_home"])
        vs = ln.kv(r.stdout)
        commits = net.commits_in_log()
        check(int(vs["height"]) == commits[-1][0] and vs["state_root"] == commits[-1][1],
              f"verify-state {vs} vs last commit {commits[-1]}")
        print(f"    verify-state OK at height {vs['height']}")
        ok = True
        print("LIVE E2E PASSED")
        return 0
    except (Fail, ln.LocalnetError) as e:
        print(f"FAILED: {e}", file=sys.stderr)
        for name in ("yapd", "cometbft"):
            p = os.path.join(d, f"{name}.log")
            if os.path.exists(p):
                print(f"--- tail {name}.log", file=sys.stderr)
                print("\n".join(open(p).read().splitlines()[-25:]), file=sys.stderr)
        return 1
    finally:
        try:
            ln.stop(d)
        except Exception as e:  # noqa: BLE001 - cleanup must not hide the result
            print(f"cleanup: {e}", file=sys.stderr)
        if ok and not os.environ.get("YAP_LIVE_KEEP"):
            shutil.rmtree(base, ignore_errors=True)
        else:
            print(f"artifacts kept in {base}", file=sys.stderr)


if __name__ == "__main__":
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(main(*[os.path.abspath(p) for p in sys.argv[1:]]))
