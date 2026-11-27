#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Containerised distributed soak (M2.7): four validators + one full node from the real images and
compose files, at least --blocks committed blocks under a continuous load of transfers and burns, with a
seeded schedule of container faults.

    test_container_soak.py [--seed N] [--blocks 1000] [--timeout-commit 300ms] [--base DIR] [--keep]

Faults (each kind at least once, then random): SIGKILL of a yapd container, SIGKILL of a CometBFT
container, graceful stop of a validator, quorum loss (two validators stopped), SIGKILL of two CometBFT
containers at once, a network partition of one validator (docker network disconnect, 3+1), a full-node
restart. Senders' keys derive from the seed (never written); transactions are built and signed by the
independent Python reference (tools/refgen) and checked against the independent model.

At the end: every node at the same height; every common height identical on all five nodes (block hash,
header app hash, results, each yapd's own state root, each height committed exactly once per node across
all crashes and restarts); balances, nonces, supply, minted and burned of every account on every node =
model; `yap verify-state` of all five databases. Resource samples (docker stats), disk growth, block
intervals and throughput go to <base>/soak-report.json.
"""
import argparse
import hashlib
import json
import os
import random
import statistics
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dockernet as dn  # noqa: E402
from dockernet import Fail, check, wait_until, step, sh, DOCKER  # noqa: E402
import livenet  # noqa: E402
from livenet import note, FEE, YIP  # noqa: E402

sys.path.insert(0, os.path.join(dn.ROOT, "tools", "refgen"))
import tx_ref  # noqa: E402
import ed25519_ref  # noqa: E402
import bech32m_ref  # noqa: E402

SENDERS = 16
SENDER_BALANCE = 100_000 * YIP
KINDS = ["crash_yapd", "crash_cometbft", "stop_validator", "quorum_loss", "crash_two", "partition", "restart_fullnode"]


class Sender:
    def __init__(self, seed, k):
        self.seed = hashlib.sha256(b"yapchain-docker-soak/" + str(seed).encode() + b"/" + bytes([k])).digest()
        self.pub = ed25519_ref.public_key(self.seed)
        self.id = tx_ref.account_id(self.pub)
        self.addr = bech32m_ref.encode_address("tyap", self.id)
        self.next = 0
        self.home = None


def build(chain_id, s, nonce, kind, amount, to=None, expires=0):
    body = {"version": 1, "chain_id": chain_id, "sender_pubkey": s.pub, "nonce": nonce, "fee": FEE,
            "expires_at_height": expires, "memo": b"",
            "payload": {"type": "transfer", "to": to.id, "amount": amount} if kind == "transfer"
            else {"type": "burn", "amount": amount}}
    raw = tx_ref.sign_tx(body, s.seed)
    return raw.hex(), hashlib.sha256(raw).hexdigest().upper()


class Stats(threading.Thread):
    """docker stats every `period` seconds: memory and CPU of every container."""

    def __init__(self, net, period=10):
        super().__init__(daemon=True)
        self.net, self.period, self.samples, self.stop_flag = net, period, [], threading.Event()
        self.phase = "idle"

    def run(self):
        while not self.stop_flag.is_set():
            r = sh([DOCKER, "stats", "--no-stream", "--format", "{{json .}}"], check_rc=False, timeout=60)
            t = time.time()
            for line in r.stdout.splitlines():
                try:
                    x = json.loads(line)
                except ValueError:
                    continue
                if not x.get("Name", "").startswith(self.net.project + "-"):
                    continue
                self.samples.append({"t": t, "phase": self.phase, "name": x["Name"][len(self.net.project) + 1:-2],
                                     "mem_mib": parse_mem(x["MemUsage"].split("/")[0]),
                                     "cpu_pct": float(x["CPUPerc"].rstrip("%") or 0)})
            self.stop_flag.wait(self.period)


def parse_mem(s):
    s = s.strip()
    for unit, f in (("GiB", 1024), ("MiB", 1), ("KiB", 1 / 1024), ("B", 1 / 1048576)):
        if s.endswith(unit):
            return round(float(s[:-len(unit)]) * f, 1)
    return 0.0


def du(path):
    total = 0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                total += os.lstat(os.path.join(root, f)).st_blocks * 512
            except OSError:
                pass
    return total


def summarize(samples):
    out = {}
    for phase in sorted({s["phase"] for s in samples}):
        for kind in ("yapd", "cometbft"):
            xs = [s for s in samples if s["phase"] == phase and (s["name"].endswith("-yapd") == (kind == "yapd"))]
            if xs:
                mem = [s["mem_mib"] for s in xs]
                cpu = [s["cpu_pct"] for s in xs]
                out[f"{phase}/{kind}"] = {"mem_mib_median": statistics.median(mem), "mem_mib_max": max(mem),
                                          "cpu_pct_median": statistics.median(cpu), "cpu_pct_p95": sorted(cpu)[int(0.95 * (len(cpu) - 1))],
                                          "samples": len(xs)}
    return out


class Soak:
    def __init__(self, net, model, seed, report_dir):
        self.net, self.model, self.rng = net, model, random.Random(seed)
        self.senders = [Sender(seed, k) for k in range(SENDERS)]
        self.chain_id = net.info["chain_id"]
        self.txlog = open(os.path.join(report_dir, "transactions.jsonl"), "w")
        self.sent = 0
        self.faults = []

    def up_nodes(self):
        return [n for n in self.net.nodes if n.state("cometbft")["running"] and n.state("yapd")["running"]]

    def load_round(self, depth=3):
        up = self.up_nodes()
        if not up:
            return
        q = self.rng.choice(up)
        try:
            h = q.height()
        except (OSError, livenet.ln.LocalnetError):
            return
        for s in self.senders:
            try:
                committed = int(q.query("/account/" + s.addr)[1]["nonce"])
            except (OSError, livenet.ln.LocalnetError, Fail):
                continue
            if s.next < committed:
                s.next = committed
            if s.next > committed + depth + 2:      # mempool lost (restart): resend from the committed nonce
                s.next = committed
            while s.next < committed + depth:
                kind = "burn" if self.rng.random() < 0.1 else "transfer"
                amount = self.rng.randint(1, 1000) * 1000
                to = self.rng.choice([x for x in self.senders if x is not s])   # S8: to != sender
                tx_hex, th = build(self.chain_id, s, s.next, kind, amount, to if kind == "transfer" else None, h + 300)
                if th not in self.model.txs:
                    self.model.register_raw(tx_hex, kind, s.addr, amount, to_addr=to.addr if kind == "transfer" else None)
                # A sender's consecutive nonces go to one home node (another node's check-state has not
                # seen the previous nonce yet); the home changes only when that node is down.
                if s.home not in up:
                    s.home = self.rng.choice(up)
                node = s.home
                try:
                    code = int(node.broadcast(tx_hex)["code"])
                except livenet.ln.LocalnetError as e:
                    code = "cache" if "already exists in cache" in str(e) else f"error {str(e)[:60]}"
                except OSError as e:
                    code = f"unreachable {e}"
                self.txlog.write(json.dumps({"t": round(time.time(), 3), "hash": th, "sender": s.addr, "nonce": s.next,
                                             "kind": kind, "node": node.name, "answer": code}) + "\n")
                self.sent += 1
                if code not in (0, "cache") and not str(code).startswith(("error", "unreachable")):
                    if code == livenet.ERR_NONCE:   # someone else's copy got there first: resync
                        break
                    raise Fail(f"unexpected CheckTx {code} for {th[:16]}… via {node}")
                s.next += 1

    def fault(self, ev):
        net = self.net
        vals = net.validators
        v = vals[ev["node"]]
        w = vals[ev["other"]] if ev["other"] != ev["node"] else vals[(ev["node"] + 1) % 4]
        kind = ev["kind"]
        rec = {"kind": kind, "t": round(time.time(), 1), "node": v.name}
        if kind == "crash_yapd":
            v.crash("yapd")
        elif kind == "crash_cometbft":
            v.crash("cometbft")
        elif kind == "stop_validator":
            v.stop()
            self.sleep_loaded(ev["down_s"])
            v.start()
        elif kind == "quorum_loss":
            rec["other"] = w.name
            v.stop()
            w.stop()
            h0 = max(n.height() for n in self.up_nodes())
            self.sleep_loaded(ev["down_s"])
            h1 = max(n.height() for n in self.up_nodes())
            check(h1 <= h0 + 1, f"blocks without quorum: {h0} -> {h1}")
            rec["heights_during"] = [h0, h1]
            v.start()
            w.start()
        elif kind == "crash_two":
            rec["other"] = w.name
            v.crash("cometbft")
            w.crash("cometbft")
        elif kind == "partition":
            netname = net.project
            sh([DOCKER, "network", "disconnect", netname, v.container("cometbft")])
            net.event(f"partition: {v} disconnected (3+1)")
            self.sleep_loaded(ev["down_s"])
            sh([DOCKER, "network", "connect", "--ip", net.info["nodes"][v.name]["ip"], "--alias", v.name, netname,
                v.container("cometbft")])
            net.event(f"partition healed: {v} reconnected")
        elif kind == "restart_fullnode":
            fn = [n for n in net.nodes if not n.validator][0]
            rec["node"] = fn.name
            fn.stop()
            self.sleep_loaded(ev["down_s"])
            fn.start()
        self.faults.append(rec)

    def sleep_loaded(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.load_round()
            time.sleep(0.3)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=20261127)
    ap.add_argument("--blocks", type=int, default=1000)
    ap.add_argument("--timeout-commit", default="300ms")
    ap.add_argument("--base")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args(argv)
    base = os.path.abspath(a.base) if a.base else tempfile.mkdtemp(prefix="yapsoak-", dir=os.environ.get("YAP_DOCKER_TEST_DIR", "/tmp"))
    os.makedirs(base, exist_ok=True)
    rng = random.Random(a.seed)
    senders = [Sender(a.seed, k) for k in range(SENDERS)]
    extra = {s.addr: str(SENDER_BALANCE) for s in senders}
    net = dn.DockerNet(base, validators=4, full_nodes=1, project="yapsoak", timeout_commit=a.timeout_commit,
                       extra_accounts=extra, rpc_port_base=37657,
                       subnet_prefix="172.31.243")
    net.extra_accounts = {k: int(v) for k, v in extra.items()}
    model = livenet.Model(net)
    soak = Soak(net, model, a.seed, base)
    soak.senders = senders
    report = {"seed": a.seed, "blocks_target": a.blocks, "timeout_commit": a.timeout_commit}
    stats = Stats(net)
    ok = False
    t_start = time.monotonic()
    try:
        step("S1", "four validators up; idle measurements")
        net.files = ["localnet.yml"]
        report["startup_s"] = round(net.up(timeout=300), 1)
        net.wait_height(3, timeout=180)
        report["disk_start_bytes"] = {n.name: du(n.dir) for n in net.nodes}
        stats.start()
        time.sleep(35)
        step("S2", "load; full node joins from empty storage; seeded faults until the block target")
        stats.phase = "load"
        schedule_t = time.monotonic() + 40
        k = 0
        fn_added = False
        load_t0, h_load0 = time.time(), net.min_height(net.validators)
        while True:
            soak.load_round()
            h = max(n.height() for n in soak.up_nodes()) if soak.up_nodes() else 0
            if not fn_added and h >= 60:
                net.files = ["localnet.yml", "fullnode.yml"]
                t0 = time.monotonic()
                net.up(services=["fullnode0-yapd", "fullnode0"])
                fn = net.nodes[-1]
                wait_until(lambda: not fn.catching_up() and fn.height() >= net.min_height(net.validators) - 2, 300,
                           "full node synced")
                report["fullnode_sync_s"] = round(time.monotonic() - t0, 1)
                report["fullnode_sync_to_height"] = fn.height()
                fn_added = True
            if time.monotonic() >= schedule_t and fn_added:
                kind = KINDS[k] if k < len(KINDS) else rng.choice(KINDS)
                ev = {"kind": kind, "node": rng.randrange(4), "other": rng.randrange(4), "down_s": rng.randint(8, 18)}
                net.event(f"fault {k}: {ev}")
                soak.fault(ev)
                k += 1
                schedule_t = time.monotonic() + rng.uniform(35, 60)
            if h >= a.blocks + 5 and k >= len(KINDS):
                break
            time.sleep(0.25)
        load_t1 = time.time()
        stats.phase = "settle"
        step("S3", "heal: every container running, every node at the same height")
        for n in net.nodes:
            for which in ("yapd", "cometbft"):
                if not n.state(which)["running"]:
                    n.start(which)
        hi = net.min_height(net.validators)
        net.wait_height(hi + 3, nodes=net.nodes, timeout=300)
        top = net.min_height(net.nodes) - 1
        step("S4", f"strict comparison of heights 1..{top} on all five nodes")
        livenet.check_same_chain(net, net.nodes, top)
        model.extend(top, net.validators[0])
        for n in net.nodes:
            livenet.check_node_state(net, model, n, dn.all_accounts(net) + list(extra))
        times = []
        for h in range(2, top + 1):
            times.append(dn.yapnode.parse_rfc3339(net.validators[0].header(h)["time"]).timestamp())
        intervals = sorted(b - c for b, c in zip(times[1:], times[:-1]))
        report["blocks"] = top
        report["transactions_executed"] = len(model.included)
        report["transactions_sent"] = soak.sent
        report["load_seconds"] = round(load_t1 - load_t0, 1)
        report["load_heights"] = [h_load0, top]
        report["throughput_tx_per_s"] = round(len(model.included) / max(1, load_t1 - load_t0), 1)
        report["block_interval_s"] = {"median": round(statistics.median(intervals), 3),
                                      "p95": round(intervals[int(0.95 * (len(intervals) - 1))], 3),
                                      "max": round(intervals[-1], 1)}
        st = model.at(top)
        report["minted"], report["burned"] = st["minted"], st["burned"]
        report["faults"] = soak.faults
        stats.stop_flag.set()
        report["resources"] = summarize(stats.samples)
        report["disk_end_bytes"] = {n.name: du(n.dir) for n in net.nodes}
        report["restarts"] = {n.name: {w: n.state(w)["restarts"] for w in ("yapd", "cometbft")} for n in net.nodes}
        step("S5", "compose down; verify-state of every database; exactly-once commits")
        net.down()
        report["verify_state"] = {}
        for n in net.nodes:
            vs = net.verify_state(n)
            report["verify_state"][n.name] = vs
            roots = n.roots()
            for h in range(1, int(vs["height"]) + 1):
                check(h in roots, f"{n}: height {h} has no Commit record")
        roots_at = {n.name: report["verify_state"][n.name]["state_root"] for n in net.nodes}
        heights = {n.name: int(report["verify_state"][n.name]["height"]) for n in net.nodes}
        note(f"final heights {heights}")
        ok = True
    except Fail as e:
        print(f"FAILED: {e}", file=sys.stderr)
    finally:
        stats.stop_flag.set()
        report["ok"] = ok
        report["seconds"] = round(time.monotonic() - t_start, 1)
        report["events"] = net.events
        with open(os.path.join(base, "soak-report.json"), "w") as f:
            json.dump(report, f, indent=2, default=str)
        print(f"report: {os.path.join(base, 'soak-report.json')}")
        if ok and not a.keep:
            net.destroy()
    print("CONTAINER SOAK: " + ("OK" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
