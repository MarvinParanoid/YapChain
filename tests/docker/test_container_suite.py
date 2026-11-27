#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Container acceptance suite (M2.7): the real images (yapchain, yapchain-cometbft), the real compose
files and scripts/ops/yapnode.py, on one Docker host.

    test_container_suite.py [--keep] [--only S1,M2,...] [--base DIR]

Images must exist (python3 -I scripts/ops/yapnode.py build, or YAPCHAIN_IMAGE / COMETBFT_IMAGE).
Scenario numbers follow the M2.7 checklist (1-20). Everything is checked against the independent state
model of tests/integration/livenet.py and strictly per height; at the end every database is verified
with `yap verify-state`. Writes a JSON report (timings, heights, roots, health observations) to
<base>/report.json. Exit 0 only if every scenario passed.
"""
import argparse
import base64
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dockernet as dn  # noqa: E402
from dockernet import Fail, check, wait_until, step, sh, DOCKER  # noqa: E402
import livenet  # noqa: E402
import yapnode  # noqa: E402
from livenet import note  # noqa: E402

REPORT = {"scenarios": {}, "observations": {}}


def scenario(key, title):
    def deco(fn):
        fn.key, fn.title = key, title
        return fn
    return deco


def observe(k, v):
    REPORT["observations"][k] = v
    note(f"{k}: {v}")


# ------------------------------------------------------------------ helpers

def progress(nodes, seconds):
    """Heights before and after `seconds` on the given nodes."""
    before = {n.name: n.height() for n in nodes}
    time.sleep(seconds)
    after = {n.name: n.height() for n in nodes}
    return before, after


def wait_healthy(node, which="cometbft", timeout=180):
    return wait_until(lambda: node.state(which)["health"] == "healthy", timeout, f"{node}/{which} healthy")


def network_transfer(net, node, sender, to, amount, extra=()):
    """`yap network transfer` inside the image, against the node's RPC published on the host loopback:
    the key never leaves the wallet volume; the CLI checks the chain id, signs, sends once and waits."""
    r = net.yap("network", "--rpc", node.meta["rpc"], "--chain-id", net.info["chain_id"], "transfer", "--from", sender,
                "--to", net.addresses[to], "--amount", amount, "--wait", "--wait-timeout", "120", "--yes", "--json",
                *extra, network="host", check_rc=False)
    check(r.returncode == 0, f"network transfer via {node}: {r.returncode}\n{r.stdout}{r.stderr}")
    return json.loads(r.stdout)


def register_by_hash(model, node, h, kind, sender, amount, to=None):
    """The CLI signed and sent the transaction itself; the model learns its bytes from the node's index."""
    raw = base64.b64decode(node.rpc("tx", hash="0x" + h)["tx"])
    model.register_raw(raw.hex(), kind, sender, amount, to_addr=to)


def hardening(net):
    """Runtime configuration of every container: non-root, read-only root, no capabilities, no
    privileged mode, no Docker socket, yapd without network, RPC published on loopback only."""
    rows = []
    for n in net.running():
        for which in ("yapd", "cometbft"):
            c = n.inspect(which)
            hc = c["HostConfig"]
            user = c["Config"]["User"]
            check(user and not user.startswith("0") and user != "root", f"{n}/{which} runs as {user!r}")
            check(hc["ReadonlyRootfs"], f"{n}/{which}: root filesystem is writable")
            check(hc["CapDrop"] and "ALL" in [x.upper() for x in hc["CapDrop"]] and not hc.get("CapAdd"),
                  f"{n}/{which}: capabilities {hc.get('CapDrop')} / {hc.get('CapAdd')}")
            check(not hc["Privileged"], f"{n}/{which} is privileged")
            check("no-new-privileges:true" in (hc.get("SecurityOpt") or []), f"{n}/{which}: no-new-privileges missing")
            for m in c["Mounts"]:
                check("docker.sock" not in m["Source"], f"{n}/{which} mounts the Docker socket")
            if which == "yapd":
                check(hc["NetworkMode"] == "none", f"{n}/yapd network {hc['NetworkMode']}")
                check(not hc.get("PortBindings"), f"{n}/yapd publishes ports")
            else:
                for port, binds in (hc.get("PortBindings") or {}).items():
                    for b in binds:
                        if port.startswith("26657"):
                            check(b["HostIp"] == "127.0.0.1", f"{n}: RPC published on {b['HostIp']}")
                        check(not port.startswith("26658"), f"{n}: ABCI port published")
                cfg_ro = [m for m in c["Mounts"] if m["Destination"] == "/var/lib/cometbft/config"]
                check(cfg_ro and not cfg_ro[0]["RW"], f"{n}: CometBFT config is not mounted read-only")
            rows.append(f"{n}/{which}: user {user}, ro-root, cap-drop ALL, {hc['NetworkMode']}")
    return rows


# ------------------------------------------------------------------ single validator (A) and failure modes

def run_single(base, keep):
    net = dn.DockerNet(os.path.join(base, "single"), validators=1, project="yapsingle", rpc_port_base=36557,
                        subnet_prefix="172.31.241")
    v = net.nodes[0]
    try:
        step("1", "single-validator network (compose A): startup and block production")
        took = net.up()
        h = net.wait_height(3, timeout=120)
        st = yapnode.node_status(v.dir, v.meta["rpc"])
        check(st["validator"]["in_set"] and st["validator"]["voting_power"] == 10, f"validator status {st['validator']}")
        check(st["containers"]["yapd"]["health"] == "healthy" and st["containers"]["cometbft"]["health"] == "healthy",
              f"health {st['containers']}")
        observe("single_startup_to_healthy_s", round(took, 1))
        note(f"height {h}, power {st['validator']['voting_power']}/{st['validator']['total_power']}, progress {st['progress']}")
        REPORT["scenarios"]["1"] = "ok"

        step("18", "stale ABCI socket: (a) crash + automatic restart, (b) killed containers, socket left behind")
        sock = os.path.join(v.dir, "abci", "yapd.sock")
        ino = os.stat(sock).st_ino
        listening = v.current_log("yapd").count("listening on unix://")
        v.crash("yapd")
        wait_until(lambda: v.current_log("yapd").count("listening on unix://") > listening
                   and v.state("yapd")["health"] == "healthy", 120, "yapd restarted on the stale socket")
        check(os.stat(sock).st_ino != ino, "socket was not recreated")
        h_a = net.wait_height(h + 3, timeout=120)
        note(f"(a) automatic restart replaced the stale socket (inode {ino} -> {os.stat(sock).st_ino}); height {h_a}")
        sh([DOCKER, "kill", v.container("cometbft"), v.container("yapd")])   # a kill is a stop: no auto-restart
        wait_until(lambda: not v.alive("yapd") and not v.alive("cometbft"), 30, "containers killed")
        check(os.path.exists(sock), "the socket file should survive a SIGKILL of yapd")
        ino = os.stat(sock).st_ino
        net.up()
        h2 = net.wait_height(h_a + 2, timeout=120)
        check(os.stat(sock).st_ino != ino, "socket was not recreated after the kill")
        note(f"(b) stale socket left by a kill replaced on start; chain advanced to {h2}")
        REPORT["scenarios"]["18"] = "ok"

        step("16", "incompatible genesis / chain id: refused by the tools and by the node, no data touched")
        net.compose("stop", "--timeout", "60", "validator0", "validator0-yapd")
        before = net.verify_state(v)
        other = os.path.join(base, "single", "other-genesis.json")
        exp = yapnode.export(v.dir, reward_address=net.addresses["alice"])
        yapnode.make_genesis("yapchain-other-1", [exp], {net.addresses["bob"]: "5YAP"}, other,
                             uid=net.info["uid"], gid=net.info["gid"])
        try:
            yapnode.configure(v.dir, other, project="yapsingle")
            raise Fail("configure accepted a genesis of another chain")
        except yapnode.OpsError as e:
            note(f"configure refused: {str(e)[:120]}")
        try:
            yapnode.configure(v.dir, os.path.join(net.d, "genesis.json"), genesis_sha256="00" * 32, project="yapsingle")
            raise Fail("configure accepted a wrong genesis SHA-256")
        except yapnode.OpsError as e:
            note(f"wrong SHA-256 refused: {str(e)[:100]}")
        bad = os.path.join(base, "single", "bad-genesis.json")
        g = json.load(open(os.path.join(net.d, "genesis.json")))
        g["app_state"]["accounts"][0]["balance"] = "1"  # app_state no longer matches anything signed off
        g["consensus_params"]["abci"]["vote_extensions_enable_height"] = "5"
        json.dump(g, open(bad, "w"))
        try:
            yapnode.validate_genesis(bad)
            raise Fail("an invalid genesis was accepted")
        except yapnode.OpsError as e:
            note(f"invalid genesis refused: {str(e)[:100]}")
        # (a) Operator error that bypasses the tools: another chain's genesis copied over the node's.
        gpath = os.path.join(v.dir, "cometbft", "config", "genesis.json")
        saved = open(gpath, "rb").read()
        shutil.copyfile(other, gpath)
        net.compose("up", "-d", "validator0-yapd", "validator0")
        wait_until(lambda: "refusing to start" in v.current_log("cometbft"), 120, "entrypoint refusing the replaced genesis")
        msg = [l for l in v.current_log("cometbft").splitlines() if "refusing to start" in l][0]
        observe("replaced_genesis_refusal", msg[:200])
        net.compose("stop", "--timeout", "30", "validator0", "validator0-yapd")
        check(net.verify_state(v) == before, "database changed by a refused start")
        open(gpath, "wb").write(saved)
        # (b) Fresh CometBFT state + a populated application database of ANOTHER chain (a wrongly moved
        #     database): the node must refuse instead of initialising anything.
        alien = os.path.join(base, "single", "alien")
        yapnode.init(alien, "validator", "alien", "yapchain-other-2", net.info["uid"], net.info["gid"])
        ex2 = yapnode.export(alien, reward_address=net.addresses["alice"])
        g2 = os.path.join(base, "single", "alien-genesis.json")
        yapnode.make_genesis("yapchain-other-2", [ex2], {net.addresses["bob"]: "5YAP"}, g2,
                             uid=net.info["uid"], gid=net.info["gid"])
        yapnode.configure(alien, g2, project="yapalien", rpc_port=36567, publish_p2p=False)
        shutil.copytree(os.path.join(v.dir, "yapd", "data"), os.path.join(alien, "yapd", "data"))
        os.remove(os.path.join(alien, "yapd", "data", "yap.lock"))
        arc = ["docker", "compose", "--project-directory", dn.COMPOSE_DIR, "--env-file", os.path.join(alien, "compose.env"),
               "-p", "yapalien", "-f", os.path.join(dn.COMPOSE_DIR, "node.yml")]
        sh(arc + ["up", "-d"], check_rc=False, timeout=300)
        try:
            def refused_b():
                logs = sh(["docker", "logs", "yapalien-cometbft-1"], check_rc=False).stdout + \
                    sh(["docker", "logs", "yapalien-cometbft-1"], check_rc=False).stderr
                ylogs = sh(["docker", "logs", "yapalien-yapd-1"], check_rc=False).stderr
                return (re.search(r"(?i)(error during handshake|app block height|does not match|fail-stop)", logs + ylogs)
                        and "Commit: height" not in ylogs.split("listening on")[-1])
            wait_until(refused_b, 120, "node refusing a database of another chain")
            ylog = sh(["docker", "logs", "yapalien-yapd-1"], check_rc=False).stderr
            clog = sh(["docker", "logs", "yapalien-cometbft-1"], check_rc=False)
            clog = clog.stdout + clog.stderr
            hit = [m.group(0).strip() for m in re.finditer(r"(?im)^.*(error during handshake|app block height|does not match|fail-stop).*$", clog + "\n" + ylog)]
            observe("foreign_database_refusal", (hit[0] if hit else "(refused, no matching line)")[:200])
            st = yapnode.node_status(alien, "http://127.0.0.1:36567")
            check(not st.get("rpc_ready") or st.get("height", 0) == 0, f"node with a foreign database progressed: {st}")
        finally:
            sh(arc + ["down", "--timeout", "30"], check_rc=False)
        after = net.verify_state(v)
        check(after == before, f"database changed: {before} -> {after}")
        net.up()
        net.wait_height(int(before["height"]) + 2, timeout=120)
        note(f"original genesis back: chain continues; database untouched ({after['state_root'][:16]}…)")
        REPORT["scenarios"]["16"] = "ok"

        step("17", "missing and inaccessible volumes")
        net.compose("stop", "--timeout", "60", "validator0", "validator0-yapd")
        moved = v.dir + ".moved"
        os.rename(v.dir, moved)
        r = net.compose("up", "-d", "validator0-yapd", check_rc=False)
        check(r.returncode != 0 and ("bind source path does not exist" in r.stderr or "no such file" in r.stderr.lower()),
              f"compose up with a missing node directory: {r.returncode} {r.stderr[-300:]}")
        check(not os.path.exists(v.dir), "Docker created the missing node directory")
        note(f"missing directory: compose refuses ({r.stderr.strip().splitlines()[-1][:120]})")
        os.rename(moved, v.dir)
        ydir = os.path.join(v.dir, "yapd")
        os.chmod(ydir, 0o000)
        try:
            net.compose("up", "-d", "--no-deps", "validator0-yapd")
            wait_until(lambda: v.state("yapd")["restarts"] >= 1 or v.state("yapd").get("exit") == 3, 90,
                       "yapd refusing an unreadable home")
            ylog = v.current_log("yapd")
            check("cannot start" in ylog, f"yapd log: {ylog[-300:]}")
            check(v.state("cometbft")["running"] is False, "CometBFT started without a healthy yapd")
            note(f"unreadable yapd home: yapd exits 3 ('cannot start'), restarts {v.state('yapd')['restarts']}, CometBFT not started")
        finally:
            os.chmod(ydir, 0o700)
        t0 = time.monotonic()
        wait_until(lambda: v.state("yapd")["health"] == "healthy", 180, "yapd healthy again by its restart policy")
        observe("recovery_after_permission_fix_s", round(time.monotonic() - t0, 1))
        net.up()
        net.wait_height(int(after["height"]) + 2, timeout=180)
        note("permissions restored: the node recovers by itself, no data lost")
        REPORT["scenarios"]["17"] = "ok"
    finally:
        if not keep:
            net.destroy()


# ------------------------------------------------------------------ four validators + full node (B, C)

def run_network(base, keep):
    net = dn.DockerNet(os.path.join(base, "net4"), validators=4, full_nodes=1, project="yapnet4", rpc_port_base=36657,
                        subnet_prefix="172.31.242")
    net.files = ["localnet.yml"]  # B first; the full node joins later (C)
    v0, v1, v2, v3, fn = net.nodes
    model = livenet.Model(net)
    try:
        step("2", "four validators (compose B): startup and consensus")
        took = net.up()
        h = net.wait_height(5, nodes=net.validators, timeout=180)
        wait_until(lambda: all(n.peers() == 3 for n in net.validators), 60, "3 peers each")
        observe("four_validator_startup_to_healthy_s", round(took, 1))
        hardening(net)
        note(f"height {h}; hardening verified on {len(net.running()) * 2} containers")
        REPORT["scenarios"]["2"] = "ok"

        step("3-4", "remote CLI: sign in the image, broadcast through RPC, finalisation and balances")
        res = network_transfer(net, v0, "alice", "bob", "25YAP")
        check(res.get("status") == "finalized", f"transfer result {res}")
        register_by_hash(model, v0, res["hash"], "transfer", net.addresses["alice"], 25 * livenet.YIP,
                         net.addresses["bob"])
        tx_hex, th = net.make_tx("burn", "carol", 0, 3 * livenet.YIP)          # offline signing...
        model.register(tx_hex, "burn", "carol", 3 * livenet.YIP)
        out = os.path.join(net.base, "txs", "send.hex")
        open(out, "w").write(tx_hex + "\n")
        r = net.yap("network", "--rpc", v1.meta["rpc"], "--chain-id", net.info["chain_id"], "send", "/txs/send.hex",
                    "--wait", "--wait-timeout", "120", "--json", network="host", check_rc=False,
                    extra_mounts=[(os.path.join(net.base, "txs"), "/txs", "ro")])
        check(r.returncode == 0 and json.loads(r.stdout)["status"] == "finalized", f"network send: {r.stdout}{r.stderr}")
        hh = net.wait_included(th)
        livenet.check_node_state(net, model, v2, dn.all_accounts(net))
        note(f"transfer finalised at {res['height']}, offline-signed burn at {hh}; balances/nonces/supply = model")
        REPORT["scenarios"]["3"] = REPORT["scenarios"]["4"] = "ok"

        step("5", "identical committed roots at common heights")
        hi = net.min_height(net.validators) - 1
        livenet.check_same_chain(net, net.validators, hi)
        note(f"heights 1..{hi}: block hashes, header app hashes, results and state roots identical on 4 validators")
        REPORT["scenarios"]["5"] = "ok"

        step("6", "stop one validator: the other three continue")
        v3.stop()
        for which in ("yapd", "cometbft"):
            s = v3.state(which)
            check(s["exit"] == 0, f"{v3}/{which} graceful stop exit code {s['exit']}")
        check("received SIGTERM, stopping" in v3.current_log("yapd"), "yapd did not log the SIGTERM shutdown")
        b, a = progress([v0, v1, v2], 8)
        check(all(a[k] >= b[k] + 3 for k in a), f"no progress with 3 of 4: {b} -> {a}")
        note(f"graceful stop: exit 0/0; 3 of 4 progress {b} -> {a}")
        REPORT["scenarios"]["6"] = "ok"

        step("7/19", "stop a second validator: quorum lost; containers stay healthy; status reports stalled")
        v2.stop()
        time.sleep(3)
        b, a = progress([v0, v1], 20)
        check(all(a[k] <= b[k] + 1 for k in a), f"blocks without quorum: {b} -> {a}")
        rows = [yapnode.node_status(n.dir, n.meta["rpc"], stall_s=10) for n in (v0, v1)]
        for r in rows:
            check(r["containers"]["cometbft"]["health"] == "healthy" and r["containers"]["yapd"]["health"] == "healthy",
                  f"{r['node']}: containers unhealthy without quorum: {r['containers']}")
            check(r["progress"] == "stalled", f"{r['node']}: progress {r['progress']}")
        tx_hex, th = net.make_tx("transfer", "bob", 0, 1 * livenet.YIP, to="dave", node=v0)
        model.register(tx_hex, "transfer", "bob", 1 * livenet.YIP, to="dave")
        check(int(v0.broadcast(tx_hex)["code"]) == 0, "CheckTx without quorum")
        time.sleep(5)
        check(not net.find_tx(th, [v0, v1]), "a transaction was included without quorum")
        observe("quorum_loss_status", {r["node"]: {"health": r["containers"]["cometbft"]["health"], "progress": r["progress"],
                                                   "last_block_age_s": r["last_block_age_s"]} for r in rows})
        REPORT["scenarios"]["7"] = "ok"

        step("8", "restore quorum: progress resumes, the pending transaction executes once")
        v2.start()
        hq = net.wait_included(th, nodes=[v0, v1, v2], timeout=120)
        note(f"quorum back; pending transfer executed at {hq}")
        REPORT["scenarios"]["8"] = "ok"

        step("9/19", "restart the stopped validator after >= 120 blocks: blocksync catch-up; healthy while catching up")
        target = net.min_height([v0, v1, v2])
        net.wait_height(target + 120, nodes=[v0, v1, v2], timeout=300)
        r_before = {w: v3.state(w)["restarts"] for w in ("yapd", "cometbft")}
        v3.start()
        seen = []
        def caught():
            try:
                cu = v3.catching_up()
            except (OSError, livenet.ln.LocalnetError):
                return False
            if cu:
                # the container's own healthcheck, run while the node reports catching_up
                rc = sh([DOCKER, "exec", v3.container("cometbft"), "/usr/local/bin/cometbft-healthcheck"], check_rc=False).returncode
                seen.append(rc)
                return False
            return v3.height() >= net.min_height([v0, v1, v2]) - 1
        t0 = time.monotonic()
        wait_until(caught, 300, f"{v3} caught up", interval=0.1)
        observe("validator3_catch_up", {"blocks": net.min_height([v0, v1, v2]) - target, "seconds": round(time.monotonic() - t0, 1),
                                        "healthcheck_runs_while_catching_up": len(seen),
                                        "healthcheck_exit_codes": sorted(set(seen))})
        check(seen, "catch-up finished before the healthcheck could be observed")
        check(set(seen) == {0}, f"healthcheck failed while catching up: {seen}")
        check("Time to switch to consensus mode" in v3.current_log("cometbft"), "no blocksync on the restarted validator")
        check({w: v3.state(w)["restarts"] for w in ("yapd", "cometbft")} == r_before, "a container restarted during catch-up")
        REPORT["scenarios"]["9"] = REPORT["scenarios"]["19"] = "ok"

        step("10-11", "add a full node from empty storage (compose C); sync from genesis; broadcast through it")
        net.files = ["localnet.yml", "fullnode.yml"]
        t0 = time.monotonic()
        net.up(services=["fullnode0-yapd", "fullnode0"])
        seen_cu = []
        wait_until(lambda: (seen_cu.append(fn.catching_up()) or True) and not fn.catching_up()
                   and fn.height() >= net.min_height(net.validators) - 1, 300, "full node synced", interval=0.3)
        observe("fullnode_sync_from_genesis_s", round(time.monotonic() - t0, 1))
        observe("fullnode_sync_height", fn.height())
        check("Time to switch to consensus mode" in fn.current_log("cometbft") or any(seen_cu),
              "no evidence of blocksync on the full node")
        st = yapnode.node_status(fn.dir, fn.meta["rpc"])
        check(not st["validator"]["in_set"], "full node is in the validator set")
        res = network_transfer(net, fn, "carol", "dave", "2YAP")
        register_by_hash(model, fn, res["hash"], "transfer", net.addresses["carol"], 2 * livenet.YIP,
                         net.addresses["dave"])
        hi = net.min_height() - 1
        livenet.check_same_chain(net, net.nodes, hi, lo=max(1, hi - 30))
        for h in range(2, hi + 1):
            sigs = fn.block(h)["block"]["last_commit"]["signatures"]
            check(fn.address not in [s["validator_address"].upper() for s in sigs], f"full node signed height {h - 1}")
        livenet.check_node_state(net, model, fn, dn.all_accounts(net))
        note(f"full node synced from genesis, relayed a transfer (height {res['height']}), never signed")
        REPORT["scenarios"]["10"] = REPORT["scenarios"]["11"] = "ok"

        step("12", "application container crash (SIGKILL yapd): automatic recovery, no re-execution")
        r0 = v1.state("yapd")["restarts"]
        v1.crash("yapd")
        wait_until(lambda: v1.state("yapd")["restarts"] > r0 and v1.state("yapd")["health"] == "healthy", 180,
                   f"{v1}/yapd restarted and healthy")
        wait_until(lambda: v1.alive("cometbft") and v1.height() >= net.min_height([v0, v2, v3]) - 1, 240,
                   f"{v1} back in consensus")
        v1.roots()  # raises if any height was committed twice
        observe("app_crash_restarts", {"yapd": v1.state("yapd")["restarts"], "cometbft": v1.state("cometbft")["restarts"]})
        REPORT["scenarios"]["12"] = "ok"

        step("13", "CometBFT container crash (SIGKILL): automatic recovery, yapd keeps running")
        y_restarts = v2.state("yapd")["restarts"]
        c0 = v2.state("cometbft")["restarts"]
        v2.crash("cometbft")
        wait_until(lambda: v2.state("cometbft")["restarts"] > c0 and v2.alive("cometbft"), 120, f"{v2}/cometbft restarted")
        wait_until(lambda: v2.height() >= net.min_height([v0, v1, v3]) - 1, 180, f"{v2} caught up")
        check(v2.state("yapd")["restarts"] == y_restarts, "yapd restarted after a CometBFT crash")
        v2.roots()
        REPORT["scenarios"]["13"] = "ok"

        step("14", "complete compose down / up with the persistent node directories")
        h_before = net.min_height()
        net.down()
        took = net.up(timeout=300)
        h_after = net.wait_height(h_before + 3, timeout=180)
        check(all(n.height() >= h_before for n in net.nodes), "a node lost blocks across down/up")
        observe("down_up_restart_to_healthy_s", round(took, 1))
        for n in net.nodes:
            n.roots()
        note(f"height {h_before} before down, {h_after} after up; no height committed twice")
        REPORT["scenarios"]["14"] = "ok"

        step("15", "offline backup of a validator; isolated restore as a full node; verify-state")
        bdir = os.path.join(net.base, "backup")
        os.makedirs(bdir, exist_ok=True)
        v3.stop()
        arch = os.path.join(bdir, "validator3.tar")
        m = yapnode.backup(v3.dir, arch)
        check(not m["includes_private_keys"] and "cometbft/config/priv_validator_key.json" not in m["files"],
              "private keys in a default backup")
        v3.start()
        # backup of a running node is refused (no live LMDB copies)
        try:
            yapnode.backup(v0.dir, os.path.join(bdir, "live.tar"))
            raise Fail("backup of a running node was accepted")
        except yapnode.OpsError as e:
            note(f"running node: backup refused ({str(e)[:90]})")
        rdir = os.path.join(net.base, "restored")
        node, vs = yapnode.restore(arch, rdir, "full")
        check(node["role"] == "full" and node["validator_address"] != v3.address.lower() and
              node["validator_address"].upper() != v3.address, "restored node kept the validator key")
        check(vs == m["verify_state"], f"restored state {vs} != backup {m['verify_state']}")
        # Run the restored node isolated: its own project, its own network, no published P2P, peers unreachable.
        yapnode.configure(rdir, os.path.join(net.d, "genesis.json"), net.info["genesis_sha256"], peers=[],
                          rpc_port=36757, publish_p2p=False, project="yaprestore")
        env = os.path.join(rdir, "compose.env")
        rc = ["docker", "compose", "--project-directory", dn.COMPOSE_DIR, "--env-file", env, "-p", "yaprestore",
              "-f", os.path.join(dn.COMPOSE_DIR, "node.yml")]
        sh(rc + ["up", "-d", "--wait", "--wait-timeout", "180"], timeout=300)
        try:
            rst = yapnode.node_status(rdir, "http://127.0.0.1:36757")
            check(rst["rpc_ready"] and rst["peers"] == 0, f"restored node not isolated: {rst}")
            check(rst["height"] == int(vs["height"]), f"restored height {rst.get('height')} != {vs['height']}")
            want = v0.header(int(vs["height"]) + 1)["app_hash"].lower()
            check(vs["state_root"] == want, f"restored root {vs['state_root']} != network root {want}")
            time.sleep(5)
            check(yapnode.node_status(rdir, "http://127.0.0.1:36757")["height"] == rst["height"], "isolated node advanced")
        finally:
            sh(rc + ["down", "--timeout", "60"], check_rc=False)
        observe("backup", {"height": vs["height"], "state_root": vs["state_root"], "files": len(m["files"]),
                           "archive_bytes": os.path.getsize(arch)})
        REPORT["scenarios"]["15"] = "ok"

        step("15b", "validator disaster recovery: decommission the original, restore with a signing floor")
        v3.stop()
        h_decom = net.min_height([v0, v1, v2])   # the original can have signed nothing above this
        karch = os.path.join(bdir, "validator3-keys.tar")
        yapnode.backup(v3.dir, karch, include_keys=True)
        check(oct(os.stat(karch).st_mode & 0o777) == "0o600", "key backup is not 0600")
        v3.archive_logs()   # before the containers (and their logs) are removed
        net.compose("rm", "-f", "validator3", "validator3-yapd")
        decom = v3.dir + ".decommissioned"
        os.rename(v3.dir, decom)
        keyfile = os.path.join(bdir, "pv.json")
        with tarfile.open(karch) as t:
            open(keyfile, "wb").write(t.extractfile("node/cometbft/config/priv_validator_key.json").read())
        os.chmod(keyfile, 0o600)
        try:
            yapnode.restore(karch, v3.dir, "validator")
            raise Fail("validator restore without floor/confirmation accepted")
        except yapnode.OpsError:
            pass
        floor = net.min_height([v0, v1, v2]) + 5
        yapnode.restore(karch, v3.dir, "validator", validator_key=keyfile, signing_floor=floor, original_decommissioned=True)
        os.remove(keyfile)
        yapnode.configure(v3.dir, os.path.join(net.d, "genesis.json"), net.info["genesis_sha256"],
                          peers=[f"{net.info['nodes'][x.name]['node_id']}@{net.info['nodes'][x.name]['ip']}:26656" for x in (v0, v1, v2)],
                          rpc_port=36687, publish_p2p=False, private_network=True, peer_dial_period="2s",
                          project="yapnet4")
        net.up(services=["validator3-yapd", "validator3"])
        net.wait_height(floor + 8, nodes=[v0, v1, v2, v3], timeout=240)
        signed = []
        for h in range(2, net.min_height() + 1):
            sigs = v0.block(h)["block"]["last_commit"]["signatures"]
            if any(s["validator_address"].upper() == v3.address and int(s["block_id_flag"]) == livenet.COMMIT for s in sigs):
                signed.append(h - 1)
        after_restore = [h for h in signed if h > h_decom]
        check(after_restore and min(after_restore) > floor,
              f"restored validator signed at/below the floor {floor} (original stopped at {h_decom}): {after_restore[:5]}")
        ev = [h for h in range(1, net.min_height() + 1) if v0.block(h)["block"]["evidence"]["evidence"]]
        check(not ev, f"evidence (double signing) at heights {ev}")
        observe("validator_recovery", {"original_last_possible_height": h_decom, "floor": floor,
                                       "first_signed_after_restore": min(after_restore), "evidence": 0})
        REPORT["scenarios"]["15b"] = "ok"

        step("20", "secrets: image layers, image history, container logs")
        observe("secret_scan", secret_scan(net))
        REPORT["scenarios"]["20"] = "ok"

        step("end", "final: model for every account on every node, identical chain, verify-state of every database")
        hi = net.min_height() - 1
        livenet.check_same_chain(net, net.nodes, hi, lo=max(1, hi - 60))
        for n in net.nodes:
            livenet.check_node_state(net, model, n, dn.all_accounts(net))
        roots = {}
        net.down()
        for n in net.nodes:
            vs = net.verify_state(n)
            roots[n.name] = (int(vs["height"]), vs["state_root"])
            n.roots()
        common = min(h for h, _ in roots.values())
        st = model.at(common)
        REPORT["final"] = {"roots": roots, "minted": st["minted"], "burned": st["burned"],
                           "transactions": len(model.included)}
        note(f"verify-state ok on {len(roots)} databases; {len(model.included)} transactions executed exactly once")
        REPORT["scenarios"]["end"] = "ok"
    finally:
        if not keep:
            net.destroy()
            shutil.rmtree(os.path.join(net.base, "restored"), ignore_errors=True)
            shutil.rmtree(os.path.join(net.base, "net", "validator3.decommissioned"), ignore_errors=True)


# Private key material in any encoding a tool would write: PEM/OpenSSH/PKCS#8 private keys and PGP secret keys
# (public certificates and code that merely mentions "-----BEGIN" are not secrets).
PRIVATE_KEY_BLOCK = re.compile(rb"-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY(?: BLOCK)?-----\r?\n[A-Za-z0-9+/=:\r\n -]{40}")


def secret_scan(net):
    """docker save of both images: no key material, password, keystore or node data in any layer; no
    compiler in the runtime images; image history and environment free of secrets; no secret in any
    container log."""
    needles = {open(net.info["password_file"]).read().strip().encode()}
    for n in net.nodes:
        for f in ("priv_validator_key.json", "node_key.json"):
            p = os.path.join(n.dir, "cometbft", "config", f)
            if os.path.exists(p):
                b64 = json.load(open(p))["priv_key"]["value"]
                needles |= {b64.encode(), base64.b64decode(b64).hex().encode()}
    bad_names = re.compile(r"(priv_validator_key\.json|priv_validator_state\.json|node_key\.json|\.password$|"
                           r"keystore|/keys/|\.mdb$|addrbook\.json)")
    tools = re.compile(r"(^|/)(gcc|g\+\+|cc1plus|cc1|clang|ld|as|go|cmake|ninja|make)(-\d+)?$")
    result = {}
    for image in (yapnode.YAPCHAIN_IMAGE, yapnode.COMETBFT_IMAGE):
        work = tempfile.mkdtemp(prefix="imgscan-")
        key_like = []
        # md5 of every file the image's dpkg database records (unmodified distribution files)
        md5s = sh([DOCKER, "run", "--rm", "--network", "none", "--entrypoint", "sh", image, "-c",
                   "cat /var/lib/dpkg/info/*.md5sums"]).stdout
        dpkg_md5 = {path: md5 for md5, path in (l.split(None, 1) for l in md5s.splitlines() if l.strip())}
        try:
            tar = os.path.join(work, "img.tar")
            sh([DOCKER, "save", "-o", tar, image], timeout=600)
            files = 0
            with tarfile.open(tar) as outer:
                for m in outer.getmembers():
                    if not m.isfile():
                        continue
                    data = outer.extractfile(m).read()
                    for x in needles:
                        check(x not in data, f"{image}: a node secret occurs in {m.name}")
                    try:
                        inner = tarfile.open(fileobj=__import__("io").BytesIO(data))
                    except tarfile.TarError:
                        continue
                    for im in inner.getmembers():
                        files += 1
                        check(not bad_names.search(im.name), f"{image}: forbidden file {im.name}")
                        check(not (im.isfile() and tools.search(im.name)), f"{image}: build tool {im.name} in runtime image")
                        if im.isfile() and im.size < (64 << 20):
                            blob = inner.extractfile(im).read()
                            m = PRIVATE_KEY_BLOCK.search(blob)
                            if m is not None:
                                distro = dpkg_md5.get(im.name)
                                check(distro is not None and distro == hashlib.md5(blob).hexdigest(),
                                      f"{image}: private key block {m.group(0)[:40]!r} in {im.name} (not an unmodified distribution file)")
                                key_like.append(f"{im.name} (dpkg-owned, unmodified)")
            hist = sh([DOCKER, "history", "--no-trunc", "--format", "{{.CreatedBy}}", image]).stdout
            for word in ("PASSWORD", "SECRET", "TOKEN", "PRIVATE"):
                check(word not in hist.upper(), f"{image}: '{word}' in image history")
            cfg = json.loads(sh([DOCKER, "image", "inspect", image]).stdout)[0]["Config"]
            for e in cfg.get("Env") or []:
                check(not re.search(r"(?i)(pass|secret|token|key)=", e), f"{image}: environment {e}")
            check(cfg.get("User") == "10001:10001", f"{image}: user {cfg.get('User')}")
            result[image] = {"layer_files": files, "key_blocks_in_unmodified_distro_files": sorted(set(key_like))}
        finally:
            shutil.rmtree(work, ignore_errors=True)
    checked = 0
    for n in net.nodes:
        for which in ("yapd", "cometbft"):
            text = (n.archived if which == "yapd" else "") + n.current_log(which)
            checked += len(text)
            for x in needles:
                check(x.decode() not in text, f"{n}/{which} log contains a secret")
    result["log_bytes_checked"] = checked
    return result


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--base")
    ap.add_argument("--only", default="single,network")
    a = ap.parse_args(argv)
    base = os.path.abspath(a.base) if a.base else tempfile.mkdtemp(prefix="yapdock-", dir=os.environ.get("YAP_DOCKER_TEST_DIR", "/tmp"))
    os.makedirs(base, exist_ok=True)
    t0 = time.monotonic()
    ok = True
    try:
        if "single" in a.only:
            run_single(base, a.keep)
        if "network" in a.only:
            run_network(base, a.keep)
    except Fail as e:
        ok = False
        print(f"FAILED: {e}", file=sys.stderr)
    finally:
        REPORT["seconds"] = round(time.monotonic() - t0, 1)
        REPORT["ok"] = ok
        with open(os.path.join(base, "report.json"), "w") as f:
            json.dump(REPORT, f, indent=2)
        print(f"report: {os.path.join(base, 'report.json')}")
    print("CONTAINER SUITE: " + ("OK" if ok else "FAILED") + f" ({REPORT['seconds']} s)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
